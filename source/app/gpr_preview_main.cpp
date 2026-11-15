/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_gvid_reader.h"
#include "gpr_preview.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <list>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
using Clock=std::chrono::steady_clock;
using File=std::unique_ptr<FILE,decltype(&std::fclose)>;
static File regular_file(const std::string &path,uint64_t minimum,uint64_t maximum,size_t *size=nullptr) {
    int fd=open(path.c_str(),O_RDONLY|O_NONBLOCK);struct stat st={};
    if (fd<0) throw std::runtime_error("cannot open regular input file");
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<0 || uint64_t(st.st_size)<minimum || uint64_t(st.st_size)>maximum) {
        close(fd);throw std::runtime_error("requires a bounded regular input file");
    }
    FILE *file=fdopen(fd,"rb");
    if (!file) { close(fd);throw std::runtime_error("cannot read input file"); }
    if (size) *size=size_t(st.st_size);
    return File(file,std::fclose);
}
static double ms(Clock::duration d) { return std::chrono::duration<double,std::milli>(d).count(); }
static ptrdiff_t read_file(void *file,uint8_t *out,size_t bytes) {
    FILE *f=static_cast<FILE *>(file); size_t got=std::fread(out,1,bytes,f);
    return std::ferror(f) ? -1:ptrdiff_t(got);
}
static unsigned integer(const std::string &text) {
    size_t end=0;
    if (text.empty() || text[0]=='-') throw std::runtime_error("invalid unsigned integer");
    unsigned long v=std::stoul(text,&end);
    if (end!=text.size() || v>1000000) throw std::runtime_error("invalid/large integer");
    return unsigned(v);
}
static double percentile(std::vector<double> v,double fraction) {
    if (v.empty()) return 0; std::sort(v.begin(),v.end()); return v[size_t((v.size()-1)*fraction)];
}
class Profiles {
    struct Entry {
        std::vector<uint8_t> bytes;
        std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> plan;
        Entry(std::vector<uint8_t> data,gpr_preview_plan *p):bytes(std::move(data)),plan(p,gpr_preview_plan_destroy) {}
    };
    std::list<Entry> entries;
public:
    size_t created=0;
    gpr_preview_plan *get(const std::string &path,const gpr_gvid_stream_info &stream,const gpr_gvid_frame_view &frame) {
        const auto &video=stream.video;
        size_t size=0;auto in=regular_file(path,144,16*1024*1024,&size);std::vector<uint8_t> bytes(size);
        if (std::fread(bytes.data(),1,size,in.get())!=size || std::fgetc(in.get())!=EOF || std::ferror(in.get()))
            throw std::runtime_error("short/changing preview profile");
        auto number=[&](size_t at) { return uint32_t(bytes[at])|uint32_t(bytes[at+1])<<8|uint32_t(bytes[at+2])<<16|uint32_t(bytes[at+3])<<24; };
        if (number(8)!=video.width || number(12)!=video.height || number(16)!=video.sample_bits || std::memcmp(bytes.data()+20,video.cfa,4))
            throw std::runtime_error("profile and recording domains disagree");
        std::string binding_path=path+".bind";
        auto bound=regular_file(binding_path,52,52);uint8_t binding[52];
        if (std::fread(binding,1,52,bound.get())!=52 || std::fgetc(bound.get())!=EOF || std::ferror(bound.get()) || std::memcmp(binding,"GPFB",4))
            throw std::runtime_error("invalid frame-profile binding");
        auto word=[&](size_t at) { return uint32_t(binding[at])|uint32_t(binding[at+1])<<8|uint32_t(binding[at+2])<<16|uint32_t(binding[at+3])<<24; };
        auto wide=[&](size_t at) { return uint64_t(word(at))|(uint64_t(word(at+4))<<32); };
        if (word(4)!=1 || wide(8)!=frame.index || wide(16)!=frame.timestamp_ns || wide(24)!=frame.payload_size ||
            word(32)!=gpr_gvid_crc32(0,frame.payload,frame.payload_size) ||
            word(36)!=gpr_gvid_crc32(0,frame.metadata,frame.metadata_size) ||
            word(40)!=gpr_gvid_crc32(0,stream.metadata,stream.metadata_size) ||
            word(44)!=number(bytes.size()-4) || word(48)!=gpr_gvid_crc32(0,binding,48))
            throw std::runtime_error("profile does not belong to this frame/calibration");
        for (auto entry=entries.begin();entry!=entries.end();++entry) if (entry->bytes==bytes) {
            entries.splice(entries.begin(),entries,entry); return entries.front().plan.get();
        }
        gpr_preview_plan *plan=nullptr;
        int error=gpr_preview_plan_from_profile(&video,bytes.data(),bytes.size(),&plan);
        if (error) throw std::runtime_error("invalid calibration profile: "+std::to_string(error));
        std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> owned(plan,gpr_preview_plan_destroy);
        entries.emplace_front(std::move(bytes),owned.get()); owned.release(); ++created;
        if (entries.size()>4) entries.pop_back(); // bounded LRU, including expanded lens maps
        return entries.front().plan.get();
    }
};
int main(int argc,char **argv) {
    try {
        std::string path,directory,profile_directory; unsigned limit=0,runs=1; bool explicit_scale=false;
        gpr_preview_options options={}; gpr_preview_options_default(&options);
        for (int a=1;a<argc;++a) {
            std::string k=argv[a];
            if (k=="--help") {
                std::cout<<"gpr_preview_app --input clip.gvid [--downsample 4|8|16] [--limit 0] [--runs 1] [--output-dir new_directory]\n"
                    "  [--profile-dir prepared_directory] (calibrated sRGB, EXIF orientation, per-frame GPRP profiles)\n";
                return 0;
            }
            if (++a==argc) throw std::runtime_error("missing option value");
            std::string value=argv[a];
            if (k=="--input") path=value;
            else if (k=="--output-dir") directory=value;
            else if (k=="--profile-dir") profile_directory=value;
            else if (k=="--downsample") { options.downsample=integer(value); explicit_scale=true; }
            else if (k=="--limit") limit=integer(value);
            else if (k=="--runs") runs=integer(value);
            else throw std::runtime_error("unknown option");
        }
        if (path.empty() || !runs || runs>1000) throw std::runtime_error("input and valid runs required");
        if (!profile_directory.empty() && explicit_scale) throw std::runtime_error("calibrated reduction is part of the prepared profile");
        if (!directory.empty() && mkdir(directory.c_str(),0755)) throw std::runtime_error("output directory must be new");
        std::vector<double> decode_times,read_times; size_t inspected=0,rendered=0; bool end_verified=true;
        auto begin=Clock::now(); gpr_preview_info dimensions={}; Profiles profiles; double profile_prepare_ms=0;
        for (unsigned run=0;run<runs;++run) {
            auto input=regular_file(path,0,UINT64_MAX);
            gpr_gvid_reader *created=nullptr;
            int error=gpr_gvid_reader_create(read_file,input.get(),0,&created);
            if (error) throw std::runtime_error("invalid recording header: "+std::to_string(error));
            std::unique_ptr<gpr_gvid_reader,decltype(&gpr_gvid_reader_close)> reader(created,gpr_gvid_reader_close);
            gpr_gvid_stream_info stream={}; gpr_gvid_reader_info(reader.get(),&stream);
            if (profile_directory.empty() && gpr_preview_dimensions(&stream.video,&options,&dimensions)) throw std::runtime_error("unsupported preview options");
            std::vector<uint8_t> rgb(dimensions.row_bytes*dimensions.height);
            size_t count=0;
            for (;;) {
                auto read_begin=Clock::now(); gpr_gvid_frame_view frame={};
                error=gpr_gvid_reader_next(reader.get(),&frame);
                double read_ms=ms(Clock::now()-read_begin);
                if (error==GPR_GVID_DONE) break;
                if (error) throw std::runtime_error("invalid/truncated recording: "+std::to_string(error));
                ++inspected;
                if (!limit || count<limit) {
                    gpr_preview_plan *plan=nullptr; auto prepare_begin=Clock::now();
                    if (!profile_directory.empty()) {
                        char file[64]; std::snprintf(file,sizeof(file),"/%06llu.gprp",static_cast<unsigned long long>(frame.index));
                        plan=profiles.get(profile_directory+file,stream,frame);
                        if (gpr_preview_plan_info(plan,&dimensions)) throw std::runtime_error("invalid prepared preview");
                        rgb.resize(dimensions.row_bytes*dimensions.height);
                    }
                    double prepare_ms=ms(Clock::now()-prepare_begin); profile_prepare_ms+=prepare_ms;
                    auto start=Clock::now();
                    error=plan ? gpr_preview_plan_render(plan,frame.payload,frame.payload_size,rgb.data(),rgb.size(),dimensions.row_bytes,&dimensions):
                        gpr_preview_render(&stream.video,&options,frame.payload,frame.payload_size,rgb.data(),rgb.size(),dimensions.row_bytes,&dimensions);
                    double decode_ms=ms(Clock::now()-start);
                    if (error) throw std::runtime_error("preview decode failed: "+std::to_string(error));
                    read_times.push_back(read_ms); decode_times.push_back(decode_ms); ++rendered;
                    uint32_t crc=gpr_gvid_crc32(0,rgb.data(),rgb.size());
                    if (!directory.empty()) {
                        std::string file=directory+"/run"+std::to_string(run)+"-frame"+std::to_string(frame.index)+".ppm";
                        std::unique_ptr<FILE,decltype(&std::fclose)> out(std::fopen(file.c_str(),"wb"),std::fclose);
                        if (!out || std::fprintf(out.get(),"P6\n%u %u\n255\n",dimensions.width,dimensions.height)<0 ||
                            std::fwrite(rgb.data(),1,rgb.size(),out.get())!=rgb.size() || std::fflush(out.get()))
                            throw std::runtime_error("preview output failed");
                    }
                    std::cout<<"{\"event\":\"preview\",\"run\":"<<run<<",\"index\":"<<frame.index
                        <<",\"timestamp_ns\":"<<frame.timestamp_ns<<",\"width\":"<<dimensions.width
                        <<",\"height\":"<<dimensions.height<<",\"read_verify_ms\":"<<read_ms
                        <<",\"profile_prepare_ms\":"<<prepare_ms<<",\"calibrated\":"<<(plan ? "true":"false")
                        <<",\"decode_rgb_ms\":"<<decode_ms<<",\"rgb_crc32\":"<<crc<<"}\n";
                }
                ++count;
            }
            if (limit && count<limit) throw std::runtime_error("fewer frames than requested");
            end_verified=end_verified && bool(gpr_gvid_reader_end_verified(reader.get()));
        }
        double elapsed=ms(Clock::now()-begin);
        std::cout<<"{\"event\":\"summary\",\"schema\":\"gpr.stock_preview.v1\",\"runs\":"<<runs
            <<",\"inspected\":"<<inspected<<",\"rendered\":"<<rendered<<",\"width\":"<<dimensions.width
            <<",\"height\":"<<dimensions.height<<",\"decode_rgb_median_ms\":"<<percentile(decode_times,.5)
            <<",\"decode_rgb_p95_ms\":"<<percentile(decode_times,.95)<<",\"read_verify_median_ms\":"<<percentile(read_times,.5)
            <<",\"elapsed_s\":"<<elapsed/1000<<",\"wall_rendered_fps\":"<<rendered*1000/elapsed
            <<",\"calibrated\":"<<(!profile_directory.empty() ? "true":"false")<<",\"prepared_plans\":"<<profiles.created
            <<",\"profile_prepare_ms\":"<<profile_prepare_ms
            <<",\"complete\":true,\"end_marker_verified\":"<<(end_verified ? "true":"false")<<"}\n";
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
