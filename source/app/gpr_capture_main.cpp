/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_capture.h"
#if GPR_CAPTURE_HAS_PREVIEW
#include "gpr_preview_queue.h"
#include "gpr_preview_stream.h"
#include "vc5_decoder.h"
#endif
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <future>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>

using Clock=std::chrono::steady_clock;
static uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
static uint64_t integer(const std::string &s) {
    size_t end=0;
    if (s.empty() || s[0]=='-') throw std::runtime_error("invalid unsigned integer");
    uint64_t v=std::stoull(s,&end);
    if (end!=s.size() || v>UINT32_MAX) throw std::runtime_error("invalid/large integer");
    return v;
}
static uint64_t get(const uint8_t *p,unsigned n) {
    uint64_t v=0; for (unsigned i=0;i<n;++i) v|=uint64_t(p[i])<<(i*8); return v;
}
static std::vector<uint8_t> read_file(const std::string &path,size_t expected,uint32_t sample_bits) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if (!f || f.tellg()!=std::streamoff(expected)) throw std::runtime_error("input RAW size mismatch: "+path);
    std::vector<uint8_t> out(expected); f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(out.data()),out.size())) throw std::runtime_error("short RAW input");
    for (size_t i=0;i<expected;i+=2)
        if (get(out.data()+i,2)>((1u<<sample_bits)-1)) throw std::runtime_error("RAW sample exceeds declared bit depth");
    return out;
}
struct Completion { gpr_capture_completion frame; uint64_t done_ns; };
#if GPR_CAPTURE_HAS_PREVIEW
using Plan=std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)>;
using PreviewQueue=std::unique_ptr<gpr_preview_queue,decltype(&gpr_preview_queue_destroy)>;
struct ReconstructionArena {
    std::vector<uint8_t> bytes;
    size_t used=0,peak=0;
    void *allocate(size_t size) noexcept {
        if (used>bytes.size()) return nullptr;
        uintptr_t address=reinterpret_cast<uintptr_t>(bytes.data()+used);
        size_t padding=size_t((64-(address&63))&63);
        if (padding>bytes.size()-used || size>bytes.size()-used-padding) return nullptr;
        void *result=bytes.data()+used+padding;used+=padding+size;peak=std::max(peak,used);return result;
    }
};
static thread_local ReconstructionArena *reconstruction_arena=nullptr;
static void *reconstruction_allocate(size_t size) {return reconstruction_arena->allocate(size);}
static void reconstruction_release(void *) {} // All views expire together after queue copy.
struct PreviewCompletion {
    uint64_t index,timestamp_ns,done_ns,offered_ns,render_ns,capture_inflight;
    uint32_t source,payload_crc,rgb_crc,width,height;
    bool before_capture_finish;
};
static Plan read_profile(const std::string &path,const gpr_gvid_config &video,bool quarter=false) {
    int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK);struct stat st={};
    if (fd<0) throw std::runtime_error("cannot open preview profile");
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<144 || st.st_size>16*1024*1024) {
        close(fd);throw std::runtime_error("invalid preview profile size/type");
    }
    FILE *file=fdopen(fd,"rb");if (!file) {close(fd);throw std::runtime_error("cannot read preview profile");}
    std::unique_ptr<FILE,decltype(&fclose)> input(file,fclose);std::vector<uint8_t> bytes(size_t(st.st_size));
    if (fread(bytes.data(),1,bytes.size(),file)!=bytes.size() || fgetc(file)!=EOF || ferror(file)) throw std::runtime_error("short/changing preview profile");
    if (quarter && get(bytes.data()+24,4)!=4) throw std::runtime_error("encoder preview requires a quarter-scale profile");
    gpr_preview_plan *created=nullptr;
    if (gpr_preview_plan_from_profile(&video,bytes.data(),bytes.size(),&created)) throw std::runtime_error("invalid preview calibration/domain");
    return Plan(created,gpr_preview_plan_destroy);
}
#endif
struct Output {
    int fd=-1;
    bool sync_each=false;
    uint64_t frame_remaining=0;
    uint64_t writeback_bytes=0,bytes_written=0,writeback_until=0,writeback_requests=0;
    size_t writeback_page_bytes=0;
    std::vector<Completion> completions;
#if GPR_CAPTURE_HAS_PREVIEW
    std::vector<Plan> plans;
    PreviewQueue preview{nullptr,gpr_preview_queue_destroy};
    std::unique_ptr<std::atomic<uint32_t>[]> source_for_frame;
    size_t source_count=0;
    uint32_t preview_every=3,preview_delay_ms=0,preview_fail_after=0;
    uint64_t preview_setup_ns=0;
    int preview_offer_error=0;
    bool preview_from_encoder=false;
    std::unique_ptr<gpr_preview_stream,decltype(&gpr_preview_stream_destroy)> stream{nullptr,gpr_preview_stream_destroy};
    bool preview_sync_stages=false;
    uint64_t preview_flush_ns=0;
    gpr_rgb_buffer reconstructed[3]={};
    ReconstructionArena reconstruction[3];
    void release_planes() {for (auto &plane:reconstructed) plane={};}
    gpr_capture *capture=nullptr;
    std::atomic<bool> capture_finished{false};
    std::vector<PreviewCompletion> previews;
    static int display(void *user,const gpr_preview_frame_view *frame) {
        auto &out=*static_cast<Output *>(user);
        if (out.preview_fail_after && out.previews.size()>=out.preview_fail_after) return 1;
        uint32_t crc=gpr_gvid_crc32(0,frame->rgb,frame->size_bytes);
        if (out.preview_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(out.preview_delay_ms));
        gpr_capture_stats stats={};if (out.capture) gpr_capture_get_stats(out.capture,&stats);
        uint32_t source=out.source_for_frame[frame->index].load(std::memory_order_acquire);
        out.previews.push_back({frame->index,frame->timestamp_ns,now_ns(),frame->offered_ns,frame->render_ns,
            stats.inflight,source,frame->payload_crc32,crc,frame->image.width,frame->image.height,
            !out.capture_finished.load(std::memory_order_acquire)});
        return 0;
    }
#endif
    ~Output() {
#if GPR_CAPTURE_HAS_PREVIEW
        preview.reset();
        release_planes();
#endif
        if (fd>=0) close(fd);
    }
    static int write(void *user,const uint8_t *p,size_t n) {
        Output &out=*static_cast<Output *>(user);
        const uint8_t *cursor=p; size_t remaining=n;
        while (remaining) {
            ssize_t count=::write(out.fd,cursor,remaining);
            if (count<0 && errno==EINTR) continue;
            if (count<=0) return -1;
            cursor+=count; remaining-=size_t(count);
            out.bytes_written+=uint64_t(count);
        }
        if (n==48 && !std::memcmp(p,"FRM3",4)) {
            out.frame_remaining=get(p+24,8)+get(p+32,4);
        }
        else if (out.frame_remaining) {
            if (n>out.frame_remaining) return -1;
            out.frame_remaining-=n;
            if (!out.frame_remaining && out.sync_each && fsync(out.fd)) return -1;
#if defined(__linux__)
            // Start data writeback before dirty-page throttling builds up. This
            // is scheduling only: the final fsync still establishes durability.
            if (!out.frame_remaining && out.writeback_bytes) {
                // Leave the append page dirty so the next frame never has to
                // modify a partial page that this request put under writeback.
                uint64_t completed=out.bytes_written-out.bytes_written%out.writeback_page_bytes;
                if (completed-out.writeback_until>=out.writeback_bytes) {
                    if (completed>uint64_t(INT64_MAX)) return -1;
                    if (sync_file_range(out.fd,off64_t(out.writeback_until),
                            off64_t(completed-out.writeback_until),SYNC_FILE_RANGE_WRITE)) return -1;
                    out.writeback_until=completed;++out.writeback_requests;
                }
            }
#endif
        }
        return 0;
    }
    static void encoded(void *user,const gpr_capture_encoded_frame *frame) {
#if GPR_CAPTURE_HAS_PREVIEW
        Output &out=*static_cast<Output *>(user);
        if (!out.preview || out.preview_offer_error || frame->index%out.preview_every) return;
        if (frame->index>=out.source_count) {out.preview_offer_error=GPR_PREVIEW_INVALID;return;}
        uint32_t source=out.source_for_frame[frame->index].load(std::memory_order_acquire);
        if (out.preview_from_encoder) {
            gpr_preview_wavelet_planes planes={};
            if (out.stream) {
                out.preview_offer_error=gpr_preview_stream_finish(out.stream.get(),&planes);
                if (out.preview_offer_error) return;
            } else {
                planes.width=out.reconstructed[0].width;planes.height=out.reconstructed[0].height;
                for (unsigned c=0;c<3;++c) {planes.samples[c]=static_cast<const int16_t *>(out.reconstructed[c].buffer);planes.sample_count[c]=out.reconstructed[c].size/2;}
            }
            out.preview_offer_error=gpr_preview_queue_offer_wavelets(out.preview.get(),out.plans[source].get(),frame->index,
                frame->timestamp_ns,&planes,gpr_gvid_crc32(0,frame->payload,frame->payload_size));
            out.release_planes();
            if (out.stream) gpr_preview_stream_reset(out.stream.get());
        } else out.preview_offer_error=gpr_preview_queue_offer(out.preview.get(),out.plans[source].get(),frame->index,
            frame->timestamp_ns,frame->payload,frame->payload_size);
#else
        (void)user;(void)frame;
#endif
    }
    static void wait_preview(void *user,uint64_t index) {
#if GPR_CAPTURE_HAS_PREVIEW
        auto &out=*static_cast<Output *>(user);(void)index;
        if (!out.preview || out.preview_offer_error) return;
        uint64_t begin=now_ns();
        out.preview_offer_error=gpr_preview_queue_flush(out.preview.get());
        out.preview_flush_ns+=now_ns()-begin;
#else
        (void)user;(void)index;
#endif
    }
    static void rows(void *user,uint64_t index,uint64_t timestamp,const vc5_preview_row *row) {
#if GPR_CAPTURE_HAS_PREVIEW
        auto &out=*static_cast<Output *>(user);(void)timestamp;
        if (!out.preview || out.preview_offer_error || index%out.preview_every) return;
        // Errors latch in the stream atomically. Aggregate only after the
        // encoder joins channel workers; never race on preview_offer_error.
        gpr_preview_stream_push(out.stream.get(),row);
#else
        (void)user;(void)index;(void)timestamp;(void)row;
#endif
    }
    static void coefficients(void *user,uint64_t index,uint64_t timestamp,const vc5_preview_coefficients *view) {
#if GPR_CAPTURE_HAS_PREVIEW
        auto &out=*static_cast<Output *>(user);(void)timestamp;
        if (!out.preview || out.preview_offer_error || index%out.preview_every) return;
        out.release_planes();
        try {
            auto reconstruct=[&](unsigned c) {
                auto previous=reconstruction_arena;reconstruction_arena=&out.reconstruction[c];
                struct Restore {ReconstructionArena *previous;~Restore(){reconstruction_arena=previous;}} restore{previous};
                reconstruction_arena->used=0;
                return vc5_decoder_reconstruct_preview_channel(view,c,reconstruction_allocate,reconstruction_release,&out.reconstructed[c]);
            };
            auto green=std::async(std::launch::async,reconstruct,1);
            auto blue=std::async(std::launch::async,reconstruct,2);
            CODEC_ERROR errors[3]={reconstruct(0),green.get(),blue.get()};
            for (auto error:errors) if (error) out.preview_offer_error=GPR_PREVIEW_CODEC;
        } catch (...) {out.preview_offer_error=GPR_PREVIEW_NOMEM;}
        if (out.preview_offer_error) out.release_planes();
#else
        (void)user;(void)index;(void)timestamp;(void)view;
#endif
    }
    static void complete(void *user,const gpr_capture_completion *frame) {
        Output &out=*static_cast<Output *>(user);
        out.completions.push_back({*frame,now_ns()});
    }
};
#if GPR_CAPTURE_HAS_PREVIEW
// On every exit, stop display callbacks while their capture handle still lives.
struct PreviewBeforeCaptureDestruction {
    Output &output;
    ~PreviewBeforeCaptureDestruction() {
        if (output.preview) {gpr_preview_queue_cancel(output.preview.get());gpr_preview_queue_finish(output.preview.get());}
    }
};
#endif
static double percentile(std::vector<double> values,double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end());
    return values[size_t((values.size()-1)*fraction)];
}
static void release_replay_input(void *) {} // Fixtures remain alive through finish.
int main(int argc,char **argv) {
    try {
        gpr_capture_config cfg={}; cfg.video={4096,3072,30,1,4,14,{'R','G','G','B'}};
        cfg.max_inflight=4; cfg.workspace_bytes=192*size_t(1024*1024);
        cfg.metadata="{\"source_kind\":\"raw_fixture_replay\",\"calibration\":\"external_required\"}";
        cfg.metadata_size=std::strlen(static_cast<const char *>(cfg.metadata));
        uint32_t attempts=120,timeout=0;
        bool paced=false,leased=false,preview_from_encoder=false,preview_streamed=false,preview_sync_stages=false; Output output;
        std::string path; std::vector<std::string> inputs,metadata_paths,source_metadata;
        std::vector<std::string> preview_profiles;uint32_t preview_every=3,preview_delay=0,preview_fail_after=0;bool preview_option=false,preview_background=true;
        for (int a=1;a<argc;++a) {
            std::string k=argv[a];
            if (k=="--help") {
                std::cout<<"gpr_capture_app --input frame.raw [--input another.raw] --output clip.gvid\n"
                    "  [--width 4096 --height 3072 --cfa RGGB --sample-bits 14 --quality 4 --frames 900 --fps-num 30 --fps-den 1]\n"
                    "  [--queue 4 --workspace-mib 192 --submit-timeout-ms 0] [--paced] [--fsync-each] [--leased]\n"
                    "  [--writeback-mib 8] (Linux asynchronous data writeback; final fsync is retained)\n"
                    "  [--source-metadata calibration.json] (one per input, in input order; trusted JSON)\n";
#if GPR_CAPTURE_HAS_PREVIEW
                std::cout<<"  [--preview-profile source.gprp] (one per input; calibrated memory sink)\n"
                    "  [--preview-every 3 --preview-delay-ms 0] (delay simulates a slow display sink)\n"
                    "  [--preview-fail-after 0] (nonzero simulates failure after N accepted images)\n"
                    "  [--preview-foreground] (default is best-effort background priority)\n";
                std::cout<<"  [--preview-from-encoder] (quarter-scale retained-band preview)\n";
                std::cout<<"  [--preview-streamed] (quarter-scale bounded streaming preview)\n";
                std::cout<<"  [--preview-sync-stages] (wait for display before the next encode; same pixels)\n";
#endif
                return 0;
            }
            if (k=="--paced") { paced=true; continue; }
            if (k=="--leased") { leased=true; continue; }
            if (k=="--fsync-each") { output.sync_each=true; continue; }
            if (k=="--preview-foreground") {preview_background=false;preview_option=true;continue;}
            if (k=="--preview-from-encoder") {preview_from_encoder=true;preview_option=true;continue;}
            if (k=="--preview-streamed") {preview_from_encoder=true;preview_streamed=true;preview_option=true;continue;}
            if (k=="--preview-sync-stages") {preview_sync_stages=true;preview_option=true;continue;}
            if (++a==argc) throw std::runtime_error("missing argument: "+k);
            std::string v=argv[a];
            if (k=="--input") inputs.push_back(v);
            else if (k=="--source-metadata") metadata_paths.push_back(v);
            else if (k=="--preview-profile") preview_profiles.push_back(v);
            else if (k=="--output") path=v;
            else if (k=="--cfa") {
                if (v.size()!=4) throw std::runtime_error("CFA must have four letters");
                std::memcpy(cfg.video.cfa,v.data(),4);
            }
            else {
                uint64_t n=integer(v);
                if (k=="--width") cfg.video.width=uint32_t(n);
                else if (k=="--sample-bits") cfg.video.sample_bits=uint32_t(n);
                else if (k=="--height") cfg.video.height=uint32_t(n);
                else if (k=="--quality") cfg.video.quality=uint32_t(n);
                else if (k=="--fps-num") cfg.video.fps_num=uint32_t(n);
                else if (k=="--fps-den") cfg.video.fps_den=uint32_t(n);
                else if (k=="--queue") cfg.max_inflight=uint32_t(n);
                else if (k=="--writeback-mib") {
                    if (n>1024) throw std::runtime_error("writeback interval must be 0..1024 MiB");
#if !defined(__linux__)
                    if (n) throw std::runtime_error("asynchronous writeback requires Linux");
#else
                    if (n) {
                        long page=sysconf(_SC_PAGESIZE);
                        if (page<=0) throw std::runtime_error("cannot determine writeback page size");
                        output.writeback_page_bytes=size_t(page);
                    }
#endif
                    output.writeback_bytes=n*1024*1024;
                }
                else if (k=="--workspace-mib") {
                    if (n>2048) throw std::runtime_error("workspace too large");
                    cfg.workspace_bytes=size_t(n)*1024*1024;
                } else if (k=="--submit-timeout-ms") timeout=uint32_t(n);
                else if (k=="--frames") attempts=uint32_t(n);
                else if (k=="--preview-every") {preview_every=uint32_t(n);preview_option=true;}
                else if (k=="--preview-delay-ms") {preview_delay=uint32_t(n);preview_option=true;}
                else if (k=="--preview-fail-after") {preview_fail_after=uint32_t(n);preview_option=true;}
                else throw std::runtime_error("unknown option: "+k);
            }
        }
        if (output.sync_each && output.writeback_bytes) throw std::runtime_error("choose fsync-each or asynchronous writeback");
        if (!metadata_paths.empty()) {
            if (metadata_paths.size()!=inputs.size()) throw std::runtime_error("supply one calibration per RAW input");
            for (const auto &file:metadata_paths) {
                std::ifstream in(file,std::ios::binary|std::ios::ate);
                if (!in || in.tellg()<=0 || in.tellg()>std::streamoff(GPR_GVID_METADATA_LIMIT-256))
                    throw std::runtime_error("invalid/large calibration JSON");
                std::string data(size_t(in.tellg()),'\0'); in.seekg(0);
                if (!in.read(&data[0],data.size())) throw std::runtime_error("short calibration JSON");
                source_metadata.push_back(std::move(data));
            }
            cfg.metadata="{\"source_kind\":\"raw_fixture_replay\",\"calibration\":\"per_source_metadata\"}";
            cfg.metadata_size=std::strlen(static_cast<const char *>(cfg.metadata));
        }
        uint8_t header[64];
        if (path.empty() || inputs.empty() || inputs.size()>16 || !attempts || attempts>1000000 ||
            !cfg.max_inflight || cfg.max_inflight>16 ||
            gpr_gvid_write_header(header,&cfg.video,cfg.metadata,cfg.metadata_size)) throw std::runtime_error("invalid configuration");
        cfg.stride_bytes=cfg.video.width*2;
#if GPR_CAPTURE_HAS_PREVIEW
        if (!preview_profiles.empty()) {
            uint64_t preview_setup_begin=now_ns();
            if (preview_profiles.size()!=inputs.size() || !preview_every || preview_delay>1000)
                throw std::runtime_error("preview requires one profile per input, positive interval and sink delay <=1000 ms");
            size_t rgb_limit=0;
            for (const auto &file:preview_profiles) {
                output.plans.push_back(read_profile(file,cfg.video,preview_from_encoder));gpr_preview_info info={};gpr_preview_plan_info(output.plans.back().get(),&info);
                rgb_limit=std::max(rgb_limit,info.row_bytes*info.height);
            }
            output.preview_every=preview_every;output.preview_delay_ms=preview_delay;output.preview_fail_after=preview_fail_after;output.source_count=attempts;
            output.preview_from_encoder=preview_from_encoder;
            output.preview_sync_stages=preview_sync_stages;
            if (preview_streamed) {
                gpr_preview_stream *stream=nullptr;
                if (gpr_preview_stream_create(cfg.video.width,cfg.video.height,size_t(128)*1024*1024,&stream))
                    throw std::runtime_error("streaming preview geometry/capacity rejected");
                output.stream.reset(stream);
            } else if (preview_from_encoder) {
                size_t pixels=size_t((cfg.video.width+3)/4)*((cfg.video.height+3)/4);
                if (pixels>(size_t(64)*1024*1024-1024*1024)/16) throw std::runtime_error("encoder preview scratch exceeds per-channel 64 MiB limit");
                for (auto &arena:output.reconstruction) arena.bytes.resize(pixels*16+1024*1024,0);
            }
            output.source_for_frame.reset(new std::atomic<uint32_t>[attempts]);output.previews.reserve(attempts);
            gpr_preview_queue_config config={std::min(size_t(128)*1024*1024,size_t(cfg.video.width)*cfg.video.height*3+1024*1024),rgb_limit,Output::display,&output,uint32_t(preview_background)};
            gpr_preview_queue *created=nullptr;if (gpr_preview_queue_create(&config,&created)) throw std::runtime_error("preview queue create failed");
            output.preview.reset(created);
            output.preview_setup_ns=now_ns()-preview_setup_begin;
        } else if (preview_option) throw std::runtime_error("preview options require explicit source profiles");
#else
        if (!preview_profiles.empty() || preview_option) throw std::runtime_error("this capture build has no preview component");
#endif
        std::vector<std::vector<uint8_t>> raw;
        for (const auto &input:inputs) raw.push_back(read_file(input,size_t(cfg.stride_bytes)*cfg.video.height,cfg.video.sample_bits));
        for (const char *flag:{"GPR_STREAM_UNPACK","GPR_BAND_STREAM","GPR_CHANNEL_ASM",
                 "GPR_PIN_CHANNELS","GPR_DIRECT_BANDS","GPR_COMPACT_BANDS"}) setenv(flag,"1",1);
        output.completions.reserve(attempts);
        output.fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0644);
        if (output.fd<0) throw std::runtime_error("cannot create new output file");
        gpr_capture_callbacks callbacks={Output::write,Output::complete,&output};
        uint64_t setup_begin=now_ns(); gpr_capture *created=nullptr;
        int create_error=preview_streamed ?
            gpr_capture_create_with_encoder_rows(&cfg,&callbacks,Output::encoded,&output,Output::rows,&output,&created):
            gpr_capture_create_with_encoder_preview(&cfg,&callbacks,Output::encoded,&output,
                preview_from_encoder ? Output::coefficients:nullptr,&output,&created);
        if (create_error) throw std::runtime_error("capture create failed");
        std::unique_ptr<gpr_capture,decltype(&gpr_capture_destroy)> capture(created,gpr_capture_destroy);
#if GPR_CAPTURE_HAS_PREVIEW
        output.capture=created;PreviewBeforeCaptureDestruction preview_guard{output};
        if (output.preview_sync_stages && gpr_capture_set_encoder_wait(created,Output::wait_preview,&output))
            throw std::runtime_error("cannot configure encoder preview barrier");
#endif
        uint64_t setup_ns=now_ns()-setup_begin,begin=now_ns(),accepted=0,overruns=0,submit_ns=0;
        for (uint64_t i=0;i<attempts;++i) {
            uint64_t due=begin+i*1000000000ULL*cfg.video.fps_den/cfg.video.fps_num;
            if (paced) std::this_thread::sleep_until(Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(due))));
            auto &input=raw[i%raw.size()];
            std::string metadata="{\"source_index\":"+std::to_string(i%raw.size())+",\"attempt_index\":"+std::to_string(i);
            if (!source_metadata.empty()) metadata+=",\"raw_metadata\":"+source_metadata[i%raw.size()];
            metadata+="}";
            gpr_capture_frame frame={input.data(),input.size(),accepted,now_ns(),metadata.data(),metadata.size()};
            if (leased) frame.release=release_replay_input;
#if GPR_CAPTURE_HAS_PREVIEW
            if (output.preview) output.source_for_frame[accepted].store(uint32_t(i%raw.size()),std::memory_order_release);
#endif
            uint64_t start=now_ns(); int error=gpr_capture_submit(capture.get(),&frame,timeout);
            submit_ns+=now_ns()-start;
            if (error==GPR_CAPTURE_BUSY) ++overruns;
            else if (error) throw std::runtime_error("capture submit failed: "+std::to_string(error));
            else ++accepted;
        }
        int error=gpr_capture_finish(capture.get());
        uint64_t finish=now_ns();
#if GPR_CAPTURE_HAS_PREVIEW
        output.capture_finished.store(true,std::memory_order_release);
#endif
        if (error) throw std::runtime_error("capture finish failed: "+std::to_string(error));
        if (fsync(output.fd)) throw std::runtime_error("final fsync failed");
        uint64_t durable=now_ns();
#if GPR_CAPTURE_HAS_PREVIEW
        int preview_error=output.preview ? gpr_preview_queue_finish(output.preview.get()):0;
        uint64_t display_finish=now_ns();
#endif
        gpr_capture_stats stats={}; gpr_capture_get_stats(capture.get(),&stats);
        struct rusage usage={}; getrusage(RUSAGE_SELF,&usage);
        uint64_t peak_rss_bytes=uint64_t(usage.ru_maxrss);
#if !defined(__APPLE__)
        peak_rss_bytes*=1024;
#endif
        std::vector<double> encode,latency,write; size_t missed=0;
        for (const auto &r:output.completions) {
            double latency_s=double(r.done_ns-r.frame.timestamp_ns)*1e-9;
            bool late=uint64_t(r.done_ns-r.frame.timestamp_ns)>1000000000ULL*cfg.video.fps_den/cfg.video.fps_num;
            missed+=late; encode.push_back(r.frame.encode_ns*1e-6); latency.push_back(latency_s*1000); write.push_back(r.frame.write_ns*1e-6);
            std::cout<<"{\"event\":\"frame\",\"index\":"<<r.frame.index<<",\"timestamp_ns\":"<<r.frame.timestamp_ns
                <<",\"bytes\":"<<r.frame.payload_bytes<<",\"encode_ms\":"<<r.frame.encode_ns*1e-6
                <<",\"write_ms\":"<<r.frame.write_ns*1e-6<<",\"capture_to_written_ms\":"<<latency_s*1000
                <<",\"deadline_missed\":"<<(late ? "true":"false")<<"}\n";
        }
#if GPR_CAPTURE_HAS_PREVIEW
        if (output.preview) {
            gpr_preview_queue_stats state={};gpr_preview_queue_get_stats(output.preview.get(),&state);
            size_t before_finish=0,with_inflight=0;std::vector<double> render_ms,display_latency;
            for (const auto &r:output.previews) {
                before_finish+=r.before_capture_finish;with_inflight+=r.capture_inflight>0;
                render_ms.push_back(r.render_ns*1e-6);display_latency.push_back((r.done_ns-r.timestamp_ns)*1e-6);
                std::cout<<"{\"event\":\"preview\",\"index\":"<<r.index<<",\"timestamp_ns\":"<<r.timestamp_ns
                    <<",\"source_index\":"<<r.source<<",\"width\":"<<r.width<<",\"height\":"<<r.height
                    <<",\"payload_crc32\":"<<r.payload_crc<<",\"rgb_crc32\":"<<r.rgb_crc
                    <<",\"render_ms\":"<<r.render_ns*1e-6<<",\"capture_to_sink_ms\":"<<(r.done_ns-r.timestamp_ns)*1e-6
                    <<",\"offer_to_sink_ms\":"<<(r.done_ns-r.offered_ns)*1e-6<<",\"capture_inflight\":"<<r.capture_inflight
                    <<",\"before_capture_finish\":"<<(r.before_capture_finish ? "true":"false")<<"}\n";
            }
            std::cout<<"{\"event\":\"preview_summary\",\"schema\":\"gpr.capture_preview.v1\",\"sink\":\"memory_crc\""
                <<",\"pipeline\":\""<<(preview_from_encoder ? "encoder_wavelets":"compressed")<<"\""
                <<",\"streamed_inverse\":"<<(preview_streamed ? "true":"false")
                <<",\"stream_buffer_bytes\":"<<gpr_preview_stream_buffer_bytes(output.stream.get())
                <<",\"reconstruction_scratch_bytes\":"<<(output.reconstruction[0].bytes.size()+output.reconstruction[1].bytes.size()+output.reconstruction[2].bytes.size())
                <<",\"reconstruction_scratch_peak_bytes\":"<<(output.reconstruction[0].peak+output.reconstruction[1].peak+output.reconstruction[2].peak)
                <<",\"every\":"<<preview_every<<",\"sink_delay_ms\":"<<preview_delay<<",\"offered\":"<<state.offered
                <<",\"rendered\":"<<state.rendered<<",\"discarded\":"<<state.discarded<<",\"replaced\":"<<state.replaced
                <<",\"inflight_high_water\":"<<state.inflight_high_water<<",\"payload_peak_bytes\":"<<state.payload_peak_bytes
                <<",\"render_median_ms\":"<<percentile(render_ms,.5)<<",\"capture_to_sink_p95_ms\":"<<percentile(display_latency,.95)
                <<",\"sync_compute_stages\":"<<(output.preview_sync_stages ? "true":"false")
                <<",\"producer_flush_wait_s\":"<<output.preview_flush_ns*1e-9
                <<",\"before_capture_finish\":"<<before_finish<<",\"with_capture_inflight\":"<<with_inflight
                <<",\"drain_after_durability_s\":"<<(display_finish-durable)*1e-9
                <<",\"setup_s\":"<<output.preview_setup_ns*1e-9
                <<",\"background_requested\":"<<state.background_requested<<",\"background_applied\":"<<state.background_applied
                <<",\"error\":"<<(preview_error ? preview_error:output.preview_offer_error)<<"}\n";
        }
#endif
        std::cout<<"{\"event\":\"summary\",\"schema\":\"gpr.capture_replay.v1\",\"attempts\":"<<attempts
            <<",\"accepted\":"<<accepted<<",\"written\":"<<stats.written<<",\"overruns\":"<<overruns
            <<",\"queue\":"<<cfg.max_inflight<<",\"inflight_high_water\":"<<stats.inflight_high_water
            <<",\"leased\":"<<(leased ? "true":"false")
            <<",\"fsync_each\":"<<(output.sync_each ? "true":"false")
            <<",\"writeback_mib\":"<<output.writeback_bytes/(1024*1024)
            <<",\"writeback_requests\":"<<output.writeback_requests
            <<",\"writeback_scheduled_bytes\":"<<output.writeback_until
            <<",\"writeback_page_bytes\":"<<output.writeback_page_bytes
            <<",\"fps_num\":"<<cfg.video.fps_num<<",\"fps_den\":"<<cfg.video.fps_den<<",\"quality\":"<<cfg.video.quality
            <<",\"buffered_fps\":"<<double(stats.written)*1e9/(finish-begin)
            <<",\"durable_fps\":"<<double(stats.written)*1e9/(durable-begin)
            <<",\"encode_median_ms\":"<<percentile(encode,.5)<<",\"write_p95_ms\":"<<percentile(write,.95)
            <<",\"capture_to_written_p95_ms\":"<<percentile(latency,.95)<<",\"deadline_misses\":"<<missed
            <<",\"final_fsync_s\":"<<(durable-finish)*1e-9<<",\"submit_s\":"<<submit_ns*1e-9
            <<",\"queue_wait_s\":"<<stats.submit_wait_ns*1e-9<<",\"setup_s\":"<<setup_ns*1e-9
            <<",\"workspace_peak_bytes\":"<<stats.workspace_peak_bytes<<",\"workspace_fallback_peak_bytes\":"<<stats.workspace_fallback_peak_bytes
            <<",\"peak_rss_bytes\":"<<peak_rss_bytes
            <<",\"payload_bytes\":"<<stats.payload_bytes<<",\"complete\":true}\n";
#if GPR_CAPTURE_HAS_PREVIEW
        if (output.preview && (preview_error || output.preview_offer_error)) return 1;
#endif
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
