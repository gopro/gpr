#if HAS_ENCODER
#include "vc5_encoder.h"
#endif
#if HAS_DECODER
#include "vc5_decoder.h"
#endif
#if HAS_SDK && !NATIVE_CLIENT
#include "gpr.h"
#endif
#if HAS_CAPTURE
#include "gpr_capture.h"
#include "gpr_gvid_reader.h"
#endif
#if HAS_PREVIEW
#include "gpr_preview.h"
#include "gpr_preview_queue.h"
#include "gpr_preview_stream.h"
#endif
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" int c_header_probe(void);
static void check(bool okay,const char *message) { if (!okay) throw std::runtime_error(message); }
static std::vector<uint8_t> read(const std::string &path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate); check(bool(f),"fixture missing");
    std::vector<uint8_t> bytes(size_t(f.tellg())); f.seekg(0);
    check(bool(f.read(reinterpret_cast<char *>(bytes.data()),bytes.size())),"fixture read"); return bytes;
}
#if HAS_ENCODER || HAS_CAPTURE
static std::vector<uint16_t> input() {
    std::vector<uint16_t> raw(128*96);
    for (unsigned y=0;y<96;++y) for (unsigned x=0;x<128;++x)
        raw[y*128+x]=(x*123+y*97+x*y*29+(x^y)*37)&16383;
    return raw;
}
#endif
#if HAS_CAPTURE
struct Recording {
    std::vector<uint8_t> bytes; size_t cursor=0;
    static int write(void *user,const uint8_t *p,size_t size) {
        auto &r=*static_cast<Recording *>(user); r.bytes.insert(r.bytes.end(),p,p+size); return 0;
    }
    static ptrdiff_t read(void *user,uint8_t *out,size_t size) {
        auto &r=*static_cast<Recording *>(user); size=std::min(size,r.bytes.size()-r.cursor);
        std::memcpy(out,r.bytes.data()+r.cursor,size); r.cursor+=size; return ptrdiff_t(size);
    }
};
#endif
#if HAS_PREVIEW
struct Display {
    std::vector<uint8_t> rgb;
    uint64_t index=0,timestamp=0;
    static int accept(void *user,const gpr_preview_frame_view *frame) {
        auto &display=*static_cast<Display *>(user);
        display.rgb.assign(frame->rgb,frame->rgb+frame->size_bytes);
        display.index=frame->index;display.timestamp=frame->timestamp_ns;
        return 0;
    }
};
#endif
#if HAS_PREVIEW && HAS_ENCODER
struct StreamProbe {
    gpr_preview_stream *stream=nullptr;std::atomic<int> error{0};
    static void accept(void *user,const vc5_preview_row *row){auto &s=*static_cast<StreamProbe*>(user);int e=gpr_preview_stream_push(s.stream,row);if(e)s.error.store(e);}
};
#endif
int main(int argc,char **argv) {
    try {
        check((argc==2 || argc==3) && c_header_probe(),"C client/defaults");
        const std::string root=argv[1]; auto payload=read(root+"/128x96-q3.vc5");
#if HAS_ENCODER
        auto raw=input(); vc5_encoder_parameters p={}; vc5_encoder_parameters_set_default(&p);
        p.input_width=128; p.input_height=96; p.input_pitch=256;
        p.pixel_format=VC5_ENCODER_PIXEL_FORMAT_RGGB_14; p.quality_setting=VC5_ENCODER_QUALITY_SETTING_FS1;
        p.mem_alloc=std::malloc; p.mem_free=std::free;
        gpr_buffer src={raw.data(),raw.size()*2},encoded={};
        check(!vc5_encoder_process(&p,&src,&encoded,nullptr),"encoder call");
        std::unique_ptr<void,decltype(&std::free)> owner(encoded.buffer,std::free);
        check(encoded.size==payload.size() && !std::memcmp(encoded.buffer,payload.data(),payload.size()),"stock payload changed");
#endif
#if HAS_DECODER
        vc5_decoder_parameters d={}; vc5_decoder_parameters_set_default(&d);
        d.pixel_format=VC5_DECODER_PIXEL_FORMAT_RGGB_14; d.mem_alloc=std::malloc; d.mem_free=std::free;
        gpr_buffer coded={payload.data(),payload.size()},decoded={};
        check(!vc5_decoder_process(&d,&coded,&decoded,nullptr),"decoder call");
        std::unique_ptr<void,decltype(&std::free)> decoded_owner(decoded.buffer,std::free);
        check(decoded.size==128*96*2,"decoded dimensions");
#endif
#if HAS_CAPTURE
        auto capture_raw=input(); Recording recording;
        gpr_capture_config cfg={}; cfg.video={128,96,30,1,3,14,{'R','G','G','B'}};
        cfg.stride_bytes=256; cfg.max_inflight=2; cfg.workspace_bytes=4*1024*1024;
        gpr_capture_callbacks callbacks={Recording::write,nullptr,&recording}; gpr_capture *capture=nullptr;
        check(!gpr_capture_create_with_encoder_rows(&cfg,&callbacks,nullptr,nullptr,nullptr,nullptr,&capture),"capture create");
        std::unique_ptr<gpr_capture,decltype(&gpr_capture_destroy)> capture_owner(capture,gpr_capture_destroy);
        gpr_capture_frame frame={}; frame.data=capture_raw.data(); frame.size_bytes=capture_raw.size()*2; frame.timestamp_ns=123456;
        check(!gpr_capture_submit(capture,&frame,UINT32_MAX) && !gpr_capture_finish(capture),"capture submit/finish");
        gpr_gvid_reader *reader=nullptr; check(!gpr_gvid_reader_create(Recording::read,&recording,0,&reader),"reader create");
        std::unique_ptr<gpr_gvid_reader,decltype(&gpr_gvid_reader_close)> reader_owner(reader,gpr_gvid_reader_close);
        gpr_gvid_frame_view view={}; check(gpr_gvid_reader_next(reader,&view)==GPR_GVID_FRAME,"reader frame");
        check(view.timestamp_ns==123456 && view.payload_size==payload.size() &&
            !std::memcmp(view.payload,payload.data(),payload.size()),"recorded payload/timestamp");
        check(gpr_gvid_reader_next(reader,&view)==GPR_GVID_DONE && gpr_gvid_reader_end_verified(reader),"reader end");
#endif
#if HAS_PREVIEW
        gpr_gvid_config video={128,96,30,1,3,14,{'R','G','G','B'}};
        gpr_preview_options options={}; gpr_preview_options_default(&options); gpr_preview_info info={};
        std::vector<uint8_t> rgb(32*24*3);
        check(!gpr_preview_render(&video,&options,payload.data(),payload.size(),rgb.data(),rgb.size(),96,&info),"preview call");
        check(rgb==read(root+"/128x96-q3-s4.rgb"),"stock RGB changed");
        gpr_preview_calibration calibration={};gpr_preview_calibration_identity(&video,&calibration);
        calibration.orientation=6;gpr_preview_plan *created_plan=nullptr;
        check(!gpr_preview_plan_create(&video,&calibration,&created_plan),"installed calibrated plan");
        std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> plan(created_plan,gpr_preview_plan_destroy);
        check(!gpr_preview_plan_info(plan.get(),&info) && info.width==24 && info.height==32,"installed oriented dimensions");
        size_t pitch=info.row_bytes+7;rgb.assign(pitch*info.height,0xcd);
        check(!gpr_preview_plan_render(plan.get(),payload.data(),payload.size(),rgb.data(),rgb.size(),pitch,&info),"installed calibrated render");
        for (size_t y=0;y<info.height;++y) for (size_t x=info.row_bytes;x<pitch;++x) check(rgb[y*pitch+x]==0xcd,"installed calibrated pitch");
        std::vector<uint8_t> expected;
        for (size_t y=0;y<info.height;++y) expected.insert(expected.end(),rgb.begin()+y*pitch,rgb.begin()+y*pitch+info.row_bytes);
#if HAS_ENCODER
        StreamProbe stream_probe;
        check(!gpr_preview_stream_create(128,96,1024*1024,&stream_probe.stream),"installed streaming create");
        std::unique_ptr<gpr_preview_stream,decltype(&gpr_preview_stream_destroy)> stream_owner(stream_probe.stream,gpr_preview_stream_destroy);
        gpr_buffer streamed_payload={};
        check(!vc5_encoder_process_with_preview_rows(&p,&src,&streamed_payload,nullptr,StreamProbe::accept,&stream_probe)&&!stream_probe.error,"installed row encoder");
        std::unique_ptr<void,decltype(&std::free)> streamed_owner(streamed_payload.buffer,std::free);
        check(streamed_payload.size==payload.size()&&!memcmp(streamed_payload.buffer,payload.data(),payload.size()),"installed streaming payload");
        gpr_preview_wavelet_planes streamed_planes={};check(!gpr_preview_stream_finish(stream_probe.stream,&streamed_planes),"installed streaming finish");
        std::vector<uint8_t> streamed_rgb(rgb.size(),0xcd);
        check(!gpr_preview_plan_render_wavelets(plan.get(),&streamed_planes,streamed_rgb.data(),streamed_rgb.size(),pitch,&info)&&streamed_rgb==rgb,"installed streaming calibrated pixels");
#endif
        Display display;gpr_preview_queue *created_queue=nullptr;
        gpr_preview_queue_config queue_config={payload.size(),expected.size(),Display::accept,&display,1};
        check(!gpr_preview_queue_create(&queue_config,&created_queue),"installed preview queue create");
        std::unique_ptr<gpr_preview_queue,decltype(&gpr_preview_queue_destroy)> queue(created_queue,gpr_preview_queue_destroy);
        check(!gpr_preview_queue_offer(queue.get(),plan.get(),7,123456,payload.data(),payload.size()),"installed preview queue offer");
        plan.reset(); // The installed queue owns the plan until delivery.
        check(!gpr_preview_queue_finish(queue.get()),"installed preview queue finish");
        check(display.rgb==expected && display.index==7 && display.timestamp==123456,"installed preview queue pixels/identity");
#endif
#if HAS_SDK && !NATIVE_CLIENT
        gpr_parameters parameters; gpr_parameters_set_defaults(&parameters);
        parameters.input_width=128; parameters.input_height=96; parameters.input_pitch=256; parameters.enable_preview=false;
        parameters.tuning_info.pixel_format=PIXEL_FORMAT_RGGB_14;
        parameters.tuning_info.dgain_saturation_level={16383,16383,16383,16383};
        gpr_allocator allocator={std::malloc,std::free}; gpr_buffer coded={payload.data(),payload.size()},gpr={};
        check(gpr_convert_vc5_to_gpr(&allocator,&parameters,&coded,&gpr),"SDK wrap");
        std::unique_ptr<void,decltype(&std::free)> sdk_owner(gpr.buffer,std::free);
        check(gpr_check_vc5(&gpr,std::malloc,std::free),"SDK public VC-5 classification");
        gpr_buffer extracted={}; check(gpr_convert_gpr_to_vc5(&allocator,&gpr,&extracted),"SDK unwrap");
        std::unique_ptr<void,decltype(&std::free)> extracted_owner(extracted.buffer,std::free);
        check(extracted.size==payload.size() && !std::memcmp(extracted.buffer,payload.data(),payload.size()),"SDK payload changed");
        if (argc==3) {
            std::ofstream file(argv[2],std::ios::binary);
            check(bool(file.write(static_cast<const char *>(gpr.buffer),gpr.size)),"SDK fixture write");
        }
#if GPR_READING
        gpr_buffer decoded={}; check(gpr_convert_gpr_to_raw(&allocator,&gpr,&decoded),"SDK decode");
        std::unique_ptr<void,decltype(&std::free)> raw_owner(decoded.buffer,std::free);
        check(decoded.size==128*96*2,"SDK dimensions");
#endif
        gpr_parameters_destroy(&parameters,allocator.Free);
#endif
        std::cout<<"PASS: installed public C/C++ APIs and stock fixture behavior\n"; return 0;
    } catch (const std::exception &error) { std::cerr<<error.what()<<'\n'; return 1; }
}
