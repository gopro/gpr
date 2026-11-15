/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_capture.h"
#include "vc5_encoder.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <zlib.h>

namespace {
using Clock = std::chrono::steady_clock;
uint64_t elapsed(Clock::time_point start) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
}
struct Workspace {
    std::vector<uint8_t> bytes;
    size_t offset = 0, peak = 0, fallback_bytes = 0, fallback_peak = 0;
    std::map<void *, size_t> fallback;
    explicit Workspace(size_t capacity) : bytes(capacity, 0) {}
    void *alloc(size_t n) noexcept {
        if (n > SIZE_MAX-63) return nullptr;
        size_t aligned = (n+63) & ~size_t(63);
        if (offset <= bytes.size() && aligned <= bytes.size()-offset) {
            void *result = bytes.data()+offset;
            offset += aligned; peak = std::max(peak, offset); return result;
        }
        void *result = std::malloc(n);
        if (!result) return nullptr;
        try { fallback.emplace(result, n); }
        catch (...) { std::free(result); return nullptr; }
        fallback_bytes += n; fallback_peak = std::max(fallback_peak, fallback_bytes);
        return result;
    }
    void free(void *p) noexcept {
        auto it = fallback.find(p);
        if (it != fallback.end()) {
            fallback_bytes -= it->second; std::free(p); fallback.erase(it);
        }
    }
    void reset() noexcept {
        for (const auto &entry : fallback) std::free(entry.first);
        fallback.clear(); fallback_bytes = 0; offset = 0;
    }
    ~Workspace() { reset(); }
};
thread_local Workspace *current_workspace = nullptr;
void *workspace_alloc(size_t n) { return current_workspace ? current_workspace->alloc(n) : nullptr; }
void workspace_free(void *p) { if (current_workspace) current_workspace->free(p); }
uint32_t copy_and_crc32(uint8_t *dst, const void *src, size_t size) {
    const uint8_t *p = static_cast<const uint8_t *>(src);
    uint32_t crc = 0;
    constexpr size_t kChunk = 65536;
    while (size > 0) {
        size_t n = std::min(size, kChunk);
        std::memcpy(dst, p, n);
        crc = gpr_gvid_crc32(crc, dst, n);
        dst += n;
        p += n;
        size -= n;
    }
    return crc;
}
struct Slot {
    std::vector<uint8_t> raw, payload, metadata;
    size_t payload_size = 0;
    uint32_t payload_crc = 0;
    uint64_t index = 0, timestamp = 0, encode_ns = 0;
    const uint8_t *input = nullptr;
    gpr_capture_release_fn release = nullptr;
    void *release_user = nullptr;
};
}

struct gpr_capture {
    gpr_capture_config cfg;
    gpr_capture_callbacks callbacks;
    gpr_capture_encoded_fn encoded=nullptr;
    void *encoded_user=nullptr;
    gpr_capture_preview_fn preview=nullptr;
    void *preview_user=nullptr;
    gpr_capture_preview_row_fn preview_rows=nullptr;
    void *preview_row_user=nullptr;
    gpr_capture_encoder_wait_fn encoder_wait=nullptr;
    void *encoder_wait_user=nullptr;
    bool encoder_wait_active=false;
    struct PreviewContext {gpr_capture *capture;Slot *slot;};
    static void coefficients(void *user,const vc5_preview_coefficients *view) noexcept {
        auto &context=*static_cast<PreviewContext *>(user);
        try {context.capture->preview(context.capture->preview_user,context.slot->index,context.slot->timestamp,view);}
        catch (...) {context.capture->fail(GPR_CAPTURE_NOMEM);}
    }
    static void row_coefficients(void *user,const vc5_preview_row *row) noexcept {
        auto &context=*static_cast<PreviewContext *>(user);
        try {context.capture->preview_rows(context.capture->preview_row_user,
            context.slot->index,context.slot->timestamp,row);}
        catch (...) {context.capture->fail(GPR_CAPTURE_NOMEM);}
    }
    std::vector<uint8_t> metadata;
    std::vector<Slot> slots;
    Workspace workspace;
    std::mutex mutex;
    std::timed_mutex submission;
    std::condition_variable changed;
    std::vector<size_t> available, raw_queue, encoded_queue;
    std::thread encoder, writer;
    gpr_capture_stats stats = {};
    bool closing = false, encoder_done = false, writer_done = false;
    uint64_t next_index = 0, last_timestamp = 0;

    gpr_capture(const gpr_capture_config &config, const gpr_capture_callbacks &cb)
        : cfg(config), callbacks(cb), slots(config.max_inflight), workspace(config.workspace_bytes) {
        if (config.metadata_size) metadata.assign(static_cast<const uint8_t *>(config.metadata),
                    static_cast<const uint8_t *>(config.metadata)+config.metadata_size);
        size_t raw_size = size_t(cfg.video.width)*cfg.video.height*2;
        size_t payload_bound = raw_size+raw_size/2+1024*1024;
        available.reserve(slots.size()); raw_queue.reserve(slots.size()); encoded_queue.reserve(slots.size());
        for (size_t i=0; i<slots.size(); ++i) {
            slots[i].raw.resize(raw_size);
            slots[i].payload.resize(payload_bound);
            slots[i].metadata.reserve(GPR_GVID_METADATA_LIMIT);
            available.push_back(i);
        }
    }
    void release_locked(size_t i, bool discard) {
        available.push_back(i); --stats.inflight;
        if (discard) ++stats.discarded;
        changed.notify_all();
    }
    void fail_locked(int error) {
        if (!stats.error) stats.error = error;
        closing = true;
        // Raw input leases must be released on the codec thread, outside locks.
        // That worker drains raw_queue before reporting encoder_done.
        while (!encoded_queue.empty()) { size_t i=encoded_queue.back(); encoded_queue.pop_back(); release_locked(i,true); }
        changed.notify_all();
    }
    void fail(int error) { std::lock_guard<std::mutex> lock(mutex); fail_locked(error); }
    void release_input(Slot &slot) {
        auto callback=slot.release; slot.release=nullptr;
        slot.input=nullptr;
        if (callback) callback(slot.release_user);
    }
    void encode_loop() noexcept {
        size_t owned=SIZE_MAX;
        current_workspace = &workspace;
        try {
            vc5_encoder_parameters p = {};
            vc5_encoder_parameters_set_default(&p);
            p.input_width=cfg.video.width; p.input_height=cfg.video.height; p.input_pitch=cfg.video.width*2;
            p.quality_setting=static_cast<VC5_ENCODER_QUALITY_SETTING>(cfg.video.quality);
            static const VC5_ENCODER_PIXEL_FORMAT formats[4][3] = {
                {VC5_ENCODER_PIXEL_FORMAT_RGGB_12,VC5_ENCODER_PIXEL_FORMAT_RGGB_14,VC5_ENCODER_PIXEL_FORMAT_RGGB_16},
                {VC5_ENCODER_PIXEL_FORMAT_GBRG_12,VC5_ENCODER_PIXEL_FORMAT_GBRG_14,VC5_ENCODER_PIXEL_FORMAT_GBRG_16},
                {VC5_ENCODER_PIXEL_FORMAT_GRBG_12,VC5_ENCODER_PIXEL_FORMAT_GRBG_14,VC5_ENCODER_PIXEL_FORMAT_GRBG_16},
                {VC5_ENCODER_PIXEL_FORMAT_BGGR_12,VC5_ENCODER_PIXEL_FORMAT_BGGR_14,VC5_ENCODER_PIXEL_FORMAT_BGGR_16}};
            unsigned phase=0;
            if (!std::memcmp(cfg.video.cfa,"GBRG",4)) phase=1;
            else if (!std::memcmp(cfg.video.cfa,"GRBG",4)) phase=2;
            else if (!std::memcmp(cfg.video.cfa,"BGGR",4)) phase=3;
            p.pixel_format=formats[phase][(cfg.video.sample_bits-12)/2];
            p.mem_alloc=cfg.workspace_bytes ? workspace_alloc : std::malloc;
            p.mem_free=cfg.workspace_bytes ? workspace_free : std::free;
            for (;;) {
                size_t index;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    changed.wait(lock,[&]{ return stats.error || closing || !raw_queue.empty(); });
                    if (stats.error || (closing && raw_queue.empty())) break;
                    index=raw_queue.front(); raw_queue.erase(raw_queue.begin()); owned=index;
                }
                Slot &slot=slots[index];
                gpr_buffer input={const_cast<uint8_t *>(slot.input),slot.raw.size()}, compressed={nullptr,0};
                auto start=Clock::now();
                CODEC_ERROR error;
                PreviewContext context={this,&slot};
                if (preview_rows) error=vc5_encoder_process_with_preview_rows(&p,&input,
                    &compressed,nullptr,row_coefficients,&context);
                else error=vc5_encoder_process_with_preview(&p,&input,&compressed,nullptr,
                    preview ? coefficients:nullptr,&context);
                slot.encode_ns=elapsed(start);
                bool okay=!error && compressed.buffer && compressed.size && compressed.size<=slot.payload.size();
                if (okay) {
                    slot.payload_size=compressed.size;
                    slot.payload_crc=copy_and_crc32(slot.payload.data(),compressed.buffer,compressed.size);
                }
                p.mem_free(compressed.buffer);
                workspace.reset();
                release_input(slot);
                if (okay && encoded) {
                    gpr_capture_encoded_frame frame={slot.index,slot.timestamp,slot.encode_ns,
                        slot.payload.data(),slot.payload_size};
                    encoded(encoded_user,&frame);
                }
                gpr_capture_encoder_wait_fn wait=nullptr;void *wait_user=nullptr;
                uint64_t frame_index=slot.index;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stats.workspace_peak_bytes=workspace.peak;
                    stats.workspace_fallback_peak_bytes=workspace.fallback_peak;
                    if (!okay) fail_locked(GPR_CAPTURE_CODEC);
                    if (stats.error) { release_locked(index,true); owned=SIZE_MAX; break; }
                    ++stats.encoded; stats.encode_ns+=slot.encode_ns;
                    encoded_queue.push_back(index); owned=SIZE_MAX; changed.notify_all();
                    wait=encoder_wait;wait_user=encoder_wait_user;
                    encoder_wait_active=wait!=nullptr;
                }
                if (wait) {
                    wait(wait_user,frame_index);
                    std::lock_guard<std::mutex> lock(mutex);encoder_wait_active=false;changed.notify_all();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            encoder_wait_active=false;
            fail_locked(GPR_CAPTURE_NOMEM);
        }
        // All callbacks happen on this worker, including leases discarded by
        // writer failure/cancel. Keep slots owned until callbacks have returned.
        for (;;) {
            size_t index=owned; owned=SIZE_MAX;
            if (index==SIZE_MAX) {
                std::lock_guard<std::mutex> lock(mutex);
                if (raw_queue.empty()) break;
                index=raw_queue.front(); raw_queue.erase(raw_queue.begin());
            }
            try { release_input(slots[index]); }
            catch (...) { fail(GPR_CAPTURE_INVALID); }
            std::lock_guard<std::mutex> lock(mutex);
            release_locked(index,true);
        }
        current_workspace=nullptr;
        std::lock_guard<std::mutex> lock(mutex);
        encoder_done=true; changed.notify_all();
    }
    bool emit(const void *p, size_t n, uint32_t *crc) {
        if (!n) return true;
        if (callbacks.write(callbacks.user,static_cast<const uint8_t *>(p),n)) return false;
        if (crc) *crc=gpr_gvid_crc32(*crc,p,n);
        return true;
    }
    void write_loop() noexcept {
        size_t owned=SIZE_MAX;
        bool owned_written=false;
        try {
            uint8_t header[64]; uint32_t crc=0;
            if (gpr_gvid_write_header(header,&cfg.video,metadata.data(),metadata.size()) ||
                !emit(header,64,&crc) || !emit(metadata.data(),metadata.size(),&crc)) {
                fail(GPR_CAPTURE_WRITER);
            } else {
                uint64_t count=0, timestamp=0, payload_bytes=0;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        changed.wait(lock,[&]{ return stats.error || !encoded_queue.empty() || encoder_done; });
                        if (stats.error) break;
                        if (encoded_queue.empty() && encoder_done) {
                            lock.unlock();
                            uint8_t end[48]; gpr_gvid_write_end(end,count,timestamp,payload_bytes,crc);
                            if (!emit(end,48,nullptr)) fail(GPR_CAPTURE_WRITER);
                            break;
                        }
                        owned=encoded_queue.front(); encoded_queue.erase(encoded_queue.begin()); owned_written=false;
                    }
                    Slot &slot=slots[owned];
                    auto start=Clock::now(); uint8_t frame[48];
                    bool okay=!gpr_gvid_write_frame_with_crc(frame,slot.index,slot.timestamp,
                        slot.payload_size,slot.payload_crc,slot.metadata.data(),slot.metadata.size()) &&
                        emit(frame,48,&crc) && emit(slot.metadata.data(),slot.metadata.size(),&crc) &&
                        emit(slot.payload.data(),slot.payload_size,nullptr);
                    // Reuse the cache-hot payload checksum computed during encode copy
                    // rather than scanning the multi-MB payload again on the writer thread.
                    if (okay) {
                        crc=uint32_t(crc32_combine(crc,slot.payload_crc,z_off_t(slot.payload_size)));
                    }
                    uint64_t write_ns=elapsed(start);
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (!okay) fail_locked(GPR_CAPTURE_WRITER);
                        if (okay) {
                            ++stats.written; stats.payload_bytes+=slot.payload_size; stats.write_ns+=write_ns;
                            owned_written=true;
                            ++count; timestamp=slot.timestamp; payload_bytes+=slot.payload_size;
                        }
                    }
                    if (okay && callbacks.complete) {
                        gpr_capture_completion result={slot.index,slot.timestamp,slot.payload_size,slot.encode_ns,write_ns};
                        callbacks.complete(callbacks.user,&result);
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        release_locked(owned,!okay); owned=SIZE_MAX;
                        if (stats.error) break;
                    }
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            fail_locked(GPR_CAPTURE_WRITER);
            if (owned!=SIZE_MAX) release_locked(owned,!owned_written);
        }
        std::lock_guard<std::mutex> lock(mutex);
        writer_done=true; changed.notify_all();
    }
};

extern "C" int gpr_capture_create(const gpr_capture_config *cfg,
                                    const gpr_capture_callbacks *cb, gpr_capture **out) {
    return gpr_capture_create_with_observer(cfg,cb,nullptr,nullptr,out);
}
extern "C" int gpr_capture_create_with_observer(const gpr_capture_config *cfg,
    const gpr_capture_callbacks *cb,gpr_capture_encoded_fn encoded,void *encoded_user,gpr_capture **out) {
    return gpr_capture_create_with_encoder_preview(cfg,cb,encoded,encoded_user,nullptr,nullptr,out);
}
static int create_capture(const gpr_capture_config *cfg,
    const gpr_capture_callbacks *cb,gpr_capture_encoded_fn encoded,void *encoded_user,
    gpr_capture_preview_fn preview,void *preview_user,
    gpr_capture_preview_row_fn rows,void *row_user,gpr_capture **out) {
    if (!out) return GPR_CAPTURE_INVALID;
    *out=nullptr;
    uint8_t header[64];
    if (!cfg || !cb || !cb->write || !cfg->max_inflight || cfg->max_inflight>16 ||
        cfg->video.width<48 || cfg->video.height<48 ||
        cfg->stride_bytes<size_t(cfg->video.width)*2 || cfg->stride_bytes>INT32_MAX ||
        cfg->workspace_bytes>size_t(2048)*1024*1024 ||
        gpr_gvid_write_header(header,&cfg->video,cfg->metadata,cfg->metadata_size)) return GPR_CAPTURE_INVALID;
    try {
        std::unique_ptr<gpr_capture> capture(new gpr_capture(*cfg,*cb));
        capture->encoded=encoded;capture->encoded_user=encoded_user;
        capture->preview=preview;capture->preview_user=preview_user;
        capture->preview_rows=rows;capture->preview_row_user=row_user;
        capture->encoder=std::thread(&gpr_capture::encode_loop,capture.get());
        try { capture->writer=std::thread(&gpr_capture::write_loop,capture.get()); }
        catch (...) { capture->fail(GPR_CAPTURE_NOMEM); capture->encoder.join(); throw; }
        *out=capture.release(); return GPR_CAPTURE_OK;
    } catch (...) { return GPR_CAPTURE_NOMEM; }
}
extern "C" int gpr_capture_create_with_encoder_preview(const gpr_capture_config *cfg,
    const gpr_capture_callbacks *cb,gpr_capture_encoded_fn encoded,void *encoded_user,
    gpr_capture_preview_fn preview,void *preview_user,gpr_capture **out) {
    return create_capture(cfg,cb,encoded,encoded_user,preview,preview_user,nullptr,nullptr,out);
}
extern "C" int gpr_capture_create_with_encoder_rows(const gpr_capture_config *cfg,
    const gpr_capture_callbacks *cb,gpr_capture_encoded_fn encoded,void *encoded_user,
    gpr_capture_preview_row_fn rows,void *row_user,gpr_capture **out) {
    return create_capture(cfg,cb,encoded,encoded_user,nullptr,nullptr,rows,row_user,out);
}
extern "C" int gpr_capture_submit(gpr_capture *c,const gpr_capture_frame *frame,uint32_t timeout_ms) {
    if (!c || !frame || !frame->data || frame->metadata_size>GPR_GVID_METADATA_LIMIT ||
        (frame->metadata_size && !frame->metadata) ||
        (frame->release && c->cfg.stride_bytes!=size_t(c->cfg.video.width)*2) ||
        frame->size_bytes < uint64_t(c->cfg.stride_bytes)*(c->cfg.video.height-1)+size_t(c->cfg.video.width)*2)
        return GPR_CAPTURE_INVALID;
    try {
        auto deadline=Clock::now()+std::chrono::milliseconds(timeout_ms);
        std::unique_lock<std::timed_mutex> submission(c->submission,std::defer_lock);
        if (timeout_ms==UINT32_MAX) submission.lock();
        else if (!submission.try_lock_until(deadline)) return GPR_CAPTURE_BUSY;
        std::unique_lock<std::mutex> lock(c->mutex);
        if (c->stats.error) return c->stats.error;
        if (c->closing) return GPR_CAPTURE_CLOSED;
        if (frame->index!=c->next_index || (c->next_index && frame->timestamp_ns<=c->last_timestamp))
            return GPR_CAPTURE_INVALID;
        auto wait_begin=Clock::now();
        auto ready=[&]{ return c->stats.error || c->closing || !c->available.empty(); };
        if (timeout_ms==UINT32_MAX) c->changed.wait(lock,ready);
        else if (!c->changed.wait_until(lock,deadline,ready)) return GPR_CAPTURE_BUSY;
        c->stats.submit_wait_ns+=elapsed(wait_begin);
        if (c->stats.error) return c->stats.error;
        if (c->closing) return GPR_CAPTURE_CLOSED;
        size_t index=c->available.back(); c->available.pop_back();
        ++c->stats.inflight;
        c->stats.inflight_high_water=std::max(c->stats.inflight_high_water,c->stats.inflight);
        Slot &slot=c->slots[index];
        // Copy under the mutex: fixed memory, no allocations, cancel/finish
        // observe a completely owned frame. Storage callbacks never hold it.
        size_t row_bytes=size_t(c->cfg.video.width)*2;
        const uint8_t *source=static_cast<const uint8_t *>(frame->data);
        if (frame->release) slot.input=source;
        else {
            for (uint32_t y=0;y<c->cfg.video.height;++y)
                std::memcpy(slot.raw.data()+y*row_bytes,source+size_t(y)*c->cfg.stride_bytes,row_bytes);
            slot.input=slot.raw.data();
        }
        slot.release=frame->release; slot.release_user=frame->release_user;
        slot.metadata.resize(frame->metadata_size);
        if (frame->metadata_size) std::memcpy(slot.metadata.data(),frame->metadata,frame->metadata_size);
        slot.index=frame->index; slot.timestamp=frame->timestamp_ns;
        ++c->next_index; c->last_timestamp=frame->timestamp_ns;
        ++c->stats.submitted; c->raw_queue.push_back(index); c->changed.notify_all();
        return GPR_CAPTURE_OK;
    } catch (...) { c->fail(GPR_CAPTURE_NOMEM); return GPR_CAPTURE_NOMEM; }
}
extern "C" int gpr_capture_set_encoder_wait(gpr_capture *c,gpr_capture_encoder_wait_fn wait,void *user) {
    if (!c) return GPR_CAPTURE_INVALID;
    std::lock_guard<std::mutex> lock(c->mutex);
    if (c->stats.submitted || c->closing) return GPR_CAPTURE_CLOSED;
    c->encoder_wait=wait;c->encoder_wait_user=user;return 0;
}
extern "C" int gpr_capture_flush(gpr_capture *c) {
    if (!c) return GPR_CAPTURE_INVALID;
    std::lock_guard<std::timed_mutex> submission(c->submission);
    std::unique_lock<std::mutex> lock(c->mutex);
    c->changed.wait(lock,[&]{ return c->stats.inflight==0 && !c->encoder_wait_active; });
    return c->stats.error;
}
extern "C" int gpr_capture_finish(gpr_capture *c) {
    if (!c) return GPR_CAPTURE_INVALID;
    std::lock_guard<std::timed_mutex> submission(c->submission);
    std::unique_lock<std::mutex> lock(c->mutex);
    c->closing=true; c->changed.notify_all();
    c->changed.wait(lock,[&]{ return c->encoder_done && c->writer_done; });
    return c->stats.error;
}
extern "C" void gpr_capture_cancel(gpr_capture *c) { if (c) c->fail(GPR_CAPTURE_CANCELLED); }
extern "C" void gpr_capture_destroy(gpr_capture *c) {
    if (!c) return;
    gpr_capture_finish(c);
    c->encoder.join(); c->writer.join(); delete c;
}
extern "C" int gpr_capture_get_stats(gpr_capture *c,gpr_capture_stats *out) {
    if (!c || !out) return GPR_CAPTURE_INVALID;
    std::lock_guard<std::mutex> lock(c->mutex); *out=c->stats; return 0;
}
