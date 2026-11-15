/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_preview.h"
#include "gpr_gvid_reader.h"
#include "vc5_decoder.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include <limits>
#include <memory>
#include <cstdlib>
static void serial_decoder(bool enabled) {
#ifdef _WIN32
    _putenv_s("GPR_PREVIEW_SERIAL_DECODER",enabled ? "1":"0");
#else
    setenv("GPR_PREVIEW_SERIAL_DECODER",enabled ? "1":"0",1);
#endif
}
static void check(bool value,const char *why) { if (!value) throw std::runtime_error(why); }
static std::vector<uint8_t> file(const std::string &path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate); check(bool(in),"fixture missing");
    std::vector<uint8_t> out(size_t(in.tellg())); in.seekg(0);
    check(bool(in.read(reinterpret_cast<char *>(out.data()),out.size())),"fixture read"); return out;
}
struct Memory {
    const std::vector<uint8_t> &data;
    size_t pos=0,chunk=13,fail=SIZE_MAX;
    explicit Memory(const std::vector<uint8_t> &d) : data(d) {}
    static ptrdiff_t read(void *user,uint8_t *out,size_t n) {
        auto &m=*static_cast<Memory *>(user);
        if (m.pos>=m.fail) return -1;
        size_t bytes=std::min({n,m.chunk,m.data.size()-m.pos,m.fail-m.pos});
        if (bytes) std::memcpy(out,m.data.data()+m.pos,bytes);
        m.pos+=bytes; return ptrdiff_t(bytes);
    }
};
static int parse(Memory &input,unsigned *count=nullptr) {
    gpr_gvid_reader *r=nullptr;
    int error=gpr_gvid_reader_create(Memory::read,&input,0,&r);
    if (error) return error;
    unsigned frames=0; gpr_gvid_frame_view frame={};
    while ((error=gpr_gvid_reader_next(r,&frame))==GPR_GVID_FRAME) {
        check(frame.index==frames,"reader index"); ++frames;
    }
    if (error==GPR_GVID_DONE) check(gpr_gvid_reader_end_verified(r)==1,"reader finalization");
    check(gpr_gvid_reader_next(r,&frame)==error,"reader error not sticky");
    gpr_gvid_reader_close(r); if (count) *count=frames; return error;
}
static void reader_tests(const std::string &root) {
    auto data=file(root+"/container-v3.gvid");
    for (size_t chunk:{size_t(1),size_t(7),size_t(13),size_t(4096)}) {
        Memory in(data); in.chunk=chunk; unsigned frames=0;
        check(parse(in,&frames)==GPR_GVID_DONE && frames==2,"independent container fixture");
    }
    for (size_t i=0;i<data.size();++i) {
        std::vector<uint8_t> truncated(data.begin(),data.begin()+i); Memory in(truncated);
        check(parse(in)<0,"truncated recording accepted");
        auto corrupt=data; corrupt[i]^=1; Memory bad(corrupt);
        check(parse(bad)<0,"corrupt recording accepted");
    }
    for (size_t i:{size_t(0),size_t(40),size_t(85),data.size()}) {
        Memory in(data); in.fail=i; check(parse(in)==GPR_GVID_IO,"I/O failure hidden");
    }
    auto trailing=data; trailing.push_back(0); Memory in(trailing); check(parse(in)<0,"trailing bytes accepted");
    gpr_gvid_reader *reader=nullptr; Memory capped(data);
    check(!gpr_gvid_reader_create(Memory::read,&capped,4,&reader),"limit create");
    gpr_gvid_frame_view out={}; check(gpr_gvid_reader_next(reader,&out)==GPR_GVID_LIMIT,"payload limit ignored");
    gpr_gvid_reader_close(reader);
}
static void render(const std::string &root,unsigned width,unsigned scale,bool unaligned) {
    std::string name=std::to_string(width)+"x96-q3";
    auto payload=file(root+"/"+name+".vc5"),expected=file(root+"/"+name+"-s"+std::to_string(scale)+".rgb");
    std::vector<uint8_t> scratch;
    const uint8_t *source=payload.data();
    if (unaligned) { scratch.resize(payload.size()+1); std::copy(payload.begin(),payload.end(),scratch.begin()+1); source=scratch.data()+1; }
    gpr_gvid_config cfg={width,96,30,1,3,14,{'R','G','G','B'}};
    gpr_preview_options options={}; gpr_preview_options_default(&options); options.downsample=scale;
    gpr_preview_info info={}; check(!gpr_preview_dimensions(&cfg,&options,&info),"dimensions");
    check(info.width==(width+scale-1)/scale && info.height==(96+scale-1)/scale,"full-frame dimensions");
    check(expected.size()==info.row_bytes*info.height,"reference dimensions");
    size_t pitch=info.row_bytes+11,needed=(info.height-1)*pitch+info.row_bytes;
    std::vector<uint8_t> rgb(pitch*info.height+32,0xcd);
    check(gpr_preview_render(&cfg,&options,source,payload.size(),rgb.data()+16,needed-1,pitch,&info)==GPR_PREVIEW_CAPACITY,"small output accepted");
    check(std::all_of(rgb.begin(),rgb.end(),[](uint8_t p){ return p==0xcd; }),"error modified output");
    check(!gpr_preview_render(&cfg,&options,source,payload.size(),rgb.data()+16,needed,pitch,&info),"render failed");
    for (size_t y=0;y<info.height;++y) {
        check(!std::memcmp(rgb.data()+16+y*pitch,expected.data()+y*info.row_bytes,info.row_bytes),"preview differs from independent stock RGB");
        for (size_t x=info.row_bytes;x<pitch;++x) check(rgb[16+y*pitch+x]==0xcd,"display padding overwritten");
    }
    for (size_t i=0;i<16;++i) check(rgb[i]==0xcd && rgb[rgb.size()-1-i]==0xcd,"display canary overwritten");
    // Reject bad geometry/profile before invoking the historical decoder.
    cfg.width+=8;
    std::vector<uint8_t> large(1024*1024,0xcd);
    check(gpr_preview_render(&cfg,&options,source,payload.size(),large.data(),large.size(),4096,&info)==GPR_PREVIEW_PAYLOAD,"wrong payload geometry accepted");
}
static void invalid_preview(const std::string &root) {
    auto payload=file(root+"/128x96-q3.vc5");
    gpr_gvid_config cfg={128,96,30,1,3,14,{'R','G','G','B'}};
    gpr_preview_options options={}; gpr_preview_options_default(&options); gpr_preview_info info={};
    std::vector<uint8_t> rgb(32*24*3,0xcd);
    for (size_t n=0;n<payload.size();++n)
        check(gpr_preview_render(&cfg,&options,payload.data(),n,rgb.data(),rgb.size(),96,&info)==GPR_PREVIEW_PAYLOAD,"truncated VC5 accepted");
    auto bad=payload; bad[11]^=1;
    check(gpr_preview_render(&cfg,&options,bad.data(),bad.size(),rgb.data(),rgb.size(),96,&info)==GPR_PREVIEW_PAYLOAD,"changed VC5 width accepted");
    options.gain_pow2_den[0]=32;
    check(gpr_preview_dimensions(&cfg,&options,&info)==GPR_PREVIEW_INVALID,"unsafe gain shift accepted");
    gpr_preview_options_default(&options);
    for (unsigned dimension=8;dimension<48;dimension+=8) {
        auto small=cfg;small.width=dimension;
        check(gpr_preview_dimensions(&small,&options,&info)==GPR_PREVIEW_INVALID,"unsafe preview width accepted");
        check(gpr_preview_render(&small,&options,payload.data(),payload.size(),rgb.data(),rgb.size(),96,&info)==GPR_PREVIEW_INVALID,"small width reached decoder");
        small=cfg;small.height=dimension;
        check(gpr_preview_dimensions(&small,&options,&info)==GPR_PREVIEW_INVALID,"unsafe preview height accepted");
    }
    check(std::all_of(rgb.begin(),rgb.end(),[](uint8_t p){ return p==0xcd; }),"invalid input modified RGB");
}
static uint8_t display(double value) {
    value=std::max(0.0,std::min(1.0,value));
    double encoded=value<=.0031308 ? 12.92*value:1.055*std::pow(value,1.0/2.4)-.055;
    return uint8_t(std::floor(encoded*255+.5));
}
static void calibrated_preview(const std::string &root,unsigned width,unsigned scale,unsigned orientation,bool crop,bool maps,unsigned phase,unsigned bits) {
    std::string name=std::to_string(width)+"x96-q3";
    auto payload=file(root+"/"+name+".vc5"),linear=file(root+"/"+name+"-s"+std::to_string(scale)+".rgb16be");
    gpr_gvid_config cfg={width,96,30,1,3,14,{'R','G','G','B'}};
    static const char *phases[]={"RGGB","GBRG","GRBG","BGGR"};
    check(phase<4,"invalid test CFA phase");cfg.sample_bits=bits;std::memcpy(cfg.cfa,phases[phase],4);
    gpr_preview_calibration c={}; gpr_preview_calibration_identity(&cfg,&c);
    c.downsample=scale; c.orientation=orientation; c.white=15000; c.black[0]=16.5f; c.black[1]=17.5f; c.black[2]=18.5f; c.black[3]=19.5f;
    if (phase) { c.black[0]=100.25f;c.black[1]=300.5f;c.black[2]=500.75f;c.black[3]=700.f; }
    if (bits==12) { c.white/=4;for (float &black:c.black) black/=4; }
    if (bits==16) { c.white*=4;for (float &black:c.black) black*=4; }
    const float matrix[9]={1.15f,-.1f,-.05f,-.05f,1.1f,-.05f,-.1f,-.15f,1.25f};
    std::copy(matrix,matrix+9,c.camera_to_srgb); c.exposure=1.125f;
    c.camera_white[0]=.75f; c.camera_white[2]=.5f;
    if (crop) { c.crop[0]=8; c.crop[1]=8; c.crop[2]=float(width-8); c.crop[3]=88; }
    float gains[4]={1.25f,1.25f,1.25f,1.25f};
    float attenuation[4]={.5f,.5f,.5f,.5f};
    gpr_preview_gain_map map={}; map.phase_mask=15; map.rows=map.cols=2; map.spacing_x=map.spacing_y=1;
    map.area[2]=float(width); map.area[3]=96; map.values=gains; map.value_count=4;
    gpr_preview_gain_map operations[2]={map,map}; operations[1].values=attenuation;
    bool sequential=maps && orientation==6;
    if (sequential) std::fill(gains,gains+4,2.f);
    if (maps) { c.gain_maps=operations; c.gain_map_count=sequential ? 2:1; }
    gpr_preview_plan *created=nullptr;
    check(!gpr_preview_plan_create(&cfg,&c,&created) && created,"calibrated plan creation");
    std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> plan(created,gpr_preview_plan_destroy);
    // A prepared plan cannot retain caller-owned map data.
    std::fill(gains,gains+4,0.f);
    gpr_preview_info info={}; check(!gpr_preview_plan_info(plan.get(),&info),"calibrated dimensions");
    unsigned columns=unsigned(std::ceil((c.crop[2]-c.crop[0])/scale)),rows=unsigned(std::ceil((c.crop[3]-c.crop[1])/scale));
    check(info.width==(orientation>=5 ? rows:columns) && info.height==(orientation>=5 ? columns:rows),"oriented dimensions");
    size_t pitch=info.row_bytes+13,needed=(info.height-1)*pitch+info.row_bytes;
    std::vector<uint8_t> output(pitch*info.height+32,0xcd);
    check(gpr_preview_plan_render(plan.get(),payload.data(),payload.size(),output.data()+16,needed-1,pitch,&info)==GPR_PREVIEW_CAPACITY,"calibrated capacity");
    check(std::all_of(output.begin(),output.end(),[](uint8_t p){return p==0xcd;}),"capacity failure modified display");
    auto unaligned=payload; unaligned.insert(unaligned.begin(),0);
    check(!gpr_preview_plan_render(plan.get(),unaligned.data()+1,payload.size(),output.data()+16,needed,pitch,&info),"calibrated render");
    // Compare both decoder architectures before allowing RGB8 rounding in the
    // separate floating-point reference below. Include padding and canaries.
    std::vector<uint8_t> serial_output(output.size(),0xcd);
    serial_decoder(true);
    int serial_error=gpr_preview_plan_render(plan.get(),unaligned.data()+1,payload.size(),serial_output.data()+16,needed,pitch,&info);
    serial_decoder(false);
    check(!serial_error,"serial calibrated render");
    check(serial_output==output,"parallel/fused preview differs from serial decoder");
    vc5_decoder_parameters decoder={};vc5_decoder_parameters_set_default(&decoder);
    decoder.mem_alloc=std::malloc;decoder.mem_free=std::free;decoder.rgb_bits=16;
    decoder.rgb_resolution=scale==4 ? GPR_RGB_RESOLUTION_QUARTER:
        (scale==8 ? GPR_RGB_RESOLUTION_EIGHTH:GPR_RGB_RESOLUTION_SIXTEENTH);
    gpr_buffer compressed={payload.data(),payload.size()};
    std::vector<int16_t> samples[3];gpr_preview_wavelet_planes planes={};
    for (unsigned channel=0;channel<3;++channel) {
        gpr_rgb_buffer plane={};
        check(vc5_decoder_process_wavelet_channel(&decoder,&compressed,channel,&plane)==CODEC_ERROR_OKAY,"wavelet producer decode");
        std::unique_ptr<void,decltype(&std::free)> owned(plane.buffer,std::free);
        planes.width=plane.width;planes.height=plane.height;
        samples[channel].assign(static_cast<int16_t *>(plane.buffer),static_cast<int16_t *>(plane.buffer)+plane.size/2);
        planes.samples[channel]=samples[channel].data();planes.sample_count[channel]=samples[channel].size();
    }
    std::vector<uint8_t> direct(output.size(),0xcd);
    check(!gpr_preview_plan_render_wavelets(plan.get(),&planes,direct.data()+16,needed,pitch,&info),"direct wavelet render");
    check(direct==output,"direct wavelet pixels/padding differ");
    std::fill(direct.begin(),direct.end(),0xcd);
    --planes.sample_count[2];
    check(gpr_preview_plan_render_wavelets(plan.get(),&planes,direct.data()+16,needed,pitch,&info)==GPR_PREVIEW_INVALID,"short wavelet plane accepted");
    ++planes.sample_count[2];
    check(gpr_preview_plan_render_wavelets(plan.get(),&planes,direct.data()+16,needed-1,pitch,&info)==GPR_PREVIEW_CAPACITY,"short direct output accepted");
    ++planes.width;
    check(gpr_preview_plan_render_wavelets(plan.get(),&planes,direct.data()+16,needed,pitch,&info)==GPR_PREVIEW_INVALID,"wrong wavelet geometry accepted");
    check(std::all_of(direct.begin(),direct.end(),[](uint8_t p){return p==0xcd;}),"invalid wavelet render modified output");
    unsigned full_width=(width+scale-1)/scale,full_height=(96+scale-1)/scale;
    check(linear.size()==size_t(full_width)*full_height*6,"stock linear fixture extent");
    for (unsigned y=0;y<rows;++y) for (unsigned x=0;x<columns;++x) {
        unsigned sx=std::min(full_width-1,unsigned((c.crop[0]+(x+.5)*(c.crop[2]-c.crop[0])/columns)/scale));
        unsigned sy=std::min(full_height-1,unsigned((c.crop[1]+(y+.5)*(c.crop[3]-c.crop[1])/rows)/scale));
        // Independent stock fixture is mirrored and uses big-endian UInt16.
        size_t at=(size_t(sy)*full_width+(full_width-1-sx))*6;
        // Independent fixture-side Bayer-site mapping for all four layouts.
        unsigned r=0,b=3,g1=1,g2=2;
        if (phase==1) {r=2;b=1;g1=0;g2=3;}
        else if (phase==2) {r=1;b=2;g1=0;g2=3;}
        else if (phase==3) {r=3;b=0;g1=1;g2=2;}
        double camera[3];
        for (unsigned channel=0;channel<3;++channel) {
            unsigned sample=unsigned(linear[at+channel*2])*256+linear[at+channel*2+1];
            auto site_value=[&](unsigned site) {
                double value=std::max(0.0,std::min(1.0,(sample/double(1u<<(16-bits))-c.black[site])/(c.white-c.black[site])));
                return sequential ? std::min(1.0,value*2)*.5:std::min(1.0,value*(maps ? 1.25:1));
            };
            double value=channel==0 ? site_value(r):(channel==2 ? site_value(b):.5*(site_value(g1)+site_value(g2)));
            camera[channel]=std::min(double(c.camera_white[channel]),value);
        }
        unsigned dx=x,dy=y;
        if (orientation==2) dx=columns-1-x;
        if (orientation==3) { dx=columns-1-x;dy=rows-1-y; }
        if (orientation==4) dy=rows-1-y;
        if (orientation==5) { dx=y;dy=x; }
        if (orientation==6) { dx=rows-1-y;dy=x; }
        if (orientation==7) { dx=rows-1-y;dy=columns-1-x; }
        if (orientation==8) { dx=y;dy=columns-1-x; }
        for (unsigned channel=0;channel<3;++channel) {
            double value=0;
            for (unsigned i=0;i<3;++i) value+=matrix[channel*3+i]*camera[i];
            value=std::max(0.0,std::min(1.0,value))*c.exposure;
            int actual=output[16+size_t(dy)*pitch+dx*3+channel];
            check(std::abs(actual-int(display(value)))<=1,"calibrated display differs from independent stock linear RGB");
        }
    }
    for (size_t y=0;y<info.height;++y) for (size_t x=info.row_bytes;x<pitch;++x)
        check(output[16+y*pitch+x]==0xcd,"calibrated row padding");
    for (size_t i=0;i<16;++i) check(output[i]==0xcd && output[output.size()-1-i]==0xcd,"calibrated display canary");
}
static void invalid_calibration() {
    gpr_gvid_config v={128,96,30,1,3,14,{'R','G','G','B'}};
    gpr_preview_calibration c={}; gpr_preview_calibration_identity(&v,&c);
    auto reject=[&](gpr_preview_calibration bad) { gpr_preview_plan *p=reinterpret_cast<gpr_preview_plan *>(uintptr_t(1));
        check(gpr_preview_plan_create(&v,&bad,&p)==GPR_PREVIEW_INVALID && !p,"invalid calibration accepted"); };
    auto bad=c;bad.orientation=9;reject(bad);bad=c;bad.white=0;reject(bad);
    bad=c;bad.black[1]=c.white;reject(bad);bad=c;bad.exposure=std::numeric_limits<float>::infinity();reject(bad);
    bad=c;bad.camera_to_srgb[2]=std::numeric_limits<float>::quiet_NaN();reject(bad);
    bad=c;bad.crop[0]=-1;reject(bad);bad=c;bad.gain_map_count=1;reject(bad);
    bad=c;bad.downsample=2;reject(bad);
    bad=c;bad.white=.5f;reject(bad);
    bad=c;bad.crop[2]=std::numeric_limits<float>::denorm_min();reject(bad);
}
static void profile_parser_tests() {
    gpr_gvid_config v={128,96,30,1,3,14,{'R','G','G','B'}};
    std::vector<uint8_t> bytes(144,0);
    auto put=[&](size_t offset,uint32_t value) { for (unsigned i=0;i<4;++i) bytes[offset+i]=uint8_t(value>>(i*8)); };
    auto real=[&](size_t offset,float value) { uint32_t bits; std::memcpy(&bits,&value,4);put(offset,bits); };
    std::memcpy(bytes.data(),"GPRP",4); put(4,1);put(8,128);put(12,96);put(16,14);std::memcpy(bytes.data()+20,"RGGB",4);put(24,4);put(28,1);
    real(48,16383); real(52,1);real(68,1);real(84,1);real(88,1);real(92,1);real(96,1);real(100,1);
    real(112,128);real(116,96);real(128,128);real(132,96);
    put(140,gpr_gvid_crc32(0,bytes.data(),140));
    gpr_preview_plan *plan=nullptr;
    check(!gpr_preview_plan_from_profile(&v,bytes.data(),bytes.size(),&plan),"independent serialized profile");gpr_preview_plan_destroy(plan);
    for (size_t length=0;length<bytes.size();++length) {
        plan=reinterpret_cast<gpr_preview_plan *>(uintptr_t(1));
        check(gpr_preview_plan_from_profile(&v,bytes.data(),length,&plan)==GPR_PREVIEW_INVALID && !plan,"truncated GPRP accepted");
    }
    for (size_t offset=0;offset<bytes.size();++offset) {
        bytes[offset]^=1; plan=reinterpret_cast<gpr_preview_plan *>(uintptr_t(1));
        check(gpr_preview_plan_from_profile(&v,bytes.data(),bytes.size(),&plan)==GPR_PREVIEW_INVALID && !plan,"corrupt GPRP accepted");bytes[offset]^=1;
    }
    real(52,std::numeric_limits<float>::quiet_NaN()); put(140,gpr_gvid_crc32(0,bytes.data(),140));
    check(gpr_preview_plan_from_profile(&v,bytes.data(),bytes.size(),&plan)==GPR_PREVIEW_INVALID,"CRC-valid nonfinite profile accepted");
}
static void large_wavelet_rows(unsigned height) {
    gpr_gvid_config video={4096,height,20,1,3,14,{'R','G','G','B'}};
    gpr_preview_calibration calibration={};gpr_preview_calibration_identity(&video,&calibration);
    for (unsigned i=0;i<4;++i) calibration.black[i]=float(20+i*7);
    calibration.exposure=.8f;calibration.camera_to_srgb[1]=.1f;
    float values[4]={1.1f,1.3f,1.5f,1.2f};
    gpr_preview_gain_map map={};map.phase_mask=15;map.rows=map.cols=2;
    map.spacing_x=map.spacing_y=1;map.area[2]=4096;map.area[3]=float(height);
    map.values=values;map.value_count=4;
    calibration.gain_maps=&map;calibration.gain_map_count=1;
    gpr_preview_plan *created=nullptr;
    check(!gpr_preview_plan_create(&video,&calibration,&created),"large plan create");
    std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> plan(created,gpr_preview_plan_destroy);
    std::vector<int16_t> storage[3];gpr_preview_wavelet_planes planes={};planes.width=1024;planes.height=height/4;
    for (unsigned c=0;c<3;++c) {
        storage[c].resize(1024*(height/4));
        for (size_t i=0;i<storage[c].size();++i) storage[c][i]=int16_t(int((i*17+c*101)%7000)-1500);
        planes.samples[c]=storage[c].data();planes.sample_count[c]=storage[c].size();
    }
    gpr_preview_info info={};check(!gpr_preview_plan_info(plan.get(),&info),"large plan info");
    size_t pitch=info.row_bytes+13;
    std::vector<uint8_t> expected(pitch*info.height,0xcd),actual(expected.size(),0xcd);
    for (unsigned strip=0;strip<3;++strip) {
        auto crop=calibration;crop.crop[1]=float(strip*(height/3));crop.crop[3]=float((strip+1)*(height/3));
        gpr_preview_plan *part=nullptr;check(!gpr_preview_plan_create(&video,&crop,&part),"strip plan create");
        std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> holder(part,gpr_preview_plan_destroy);
        gpr_preview_info part_info={};
        // Each strip is below the parallel-display threshold and uses the same
        // global source coordinates. Stitching checks actual row partitioning.
        check(!gpr_preview_plan_render_wavelets(part,&planes,expected.data()+strip*(height/12)*pitch,
            expected.size()-strip*(height/12)*pitch,pitch,&part_info),"serial strip render");
        check(part_info.height==height/12 && part_info.width==1024,"strip geometry");
    }
    for (unsigned repeat=0;repeat<3;++repeat) {
        check(!gpr_preview_plan_render_wavelets(plan.get(),&planes,actual.data(),actual.size(),pitch,&info),"parallel large render");
        check(actual==expected,"parallel rows/padding differ from serial strips");
        std::fill(actual.begin(),actual.end(),0xcd);
    }
    bool okay[2]={false,false};std::thread callers[2];
    for (unsigned i=0;i<2;++i) callers[i]=std::thread([&,i] {
        std::vector<uint8_t> result(expected.size(),0xcd);gpr_preview_info output={};
        okay[i]=!gpr_preview_plan_render_wavelets(plan.get(),&planes,result.data(),result.size(),pitch,&output) && result==expected;
    });
    for (auto &caller:callers) caller.join();
    check(okay[0] && okay[1],"concurrent large immutable-plan render");
}
static void shared_calibrated_plan(const std::string &root) {
    gpr_gvid_config v={128,96,30,1,3,14,{'R','G','G','B'}};
    gpr_preview_calibration c={};gpr_preview_calibration_identity(&v,&c);gpr_preview_plan *created=nullptr;
    check(!gpr_preview_plan_create(&v,&c,&created),"shared plan create");
    std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)> plan(created,gpr_preview_plan_destroy);
    auto payload=file(root+"/128x96-q3.vc5");std::vector<uint8_t> results[2];int errors[2]={0,0};std::thread workers[2];
    for (unsigned i=0;i<2;++i) workers[i]=std::thread([&,i]{
        gpr_preview_info info={};gpr_preview_plan_info(plan.get(),&info);results[i].resize(info.row_bytes*info.height);
        errors[i]=gpr_preview_plan_render(plan.get(),payload.data(),payload.size(),results[i].data(),results[i].size(),info.row_bytes,&info);
    });
    for (auto &worker:workers) worker.join();
    check(!errors[0] && !errors[1] && results[0]==results[1],"concurrent immutable plan/gamma initialization");
}
int main(int argc,char **argv) {
    try {
        check(argc==2,"fixture directory required"); std::string root=argv[1]; reader_tests(root);
        // Begin concurrent renders cold, exercising curve/prefix initialization.
        bool okay[2]={true,true}; std::thread threads[2];
        for (unsigned i=0;i<2;++i) threads[i]=std::thread([&,i]{ try { render(root,128,4,i!=0); } catch (...) { okay[i]=false; } });
        for (auto &t:threads) t.join(); check(okay[0] && okay[1],"concurrent render");
        for (unsigned width:{128u,72u}) for (unsigned scale:{4u,8u,16u}) for (bool unaligned:{false,true}) render(root,width,scale,unaligned);
        invalid_preview(root);
        shared_calibrated_plan(root);
        large_wavelet_rows(3072);large_wavelet_rows(3048);
        for (unsigned width:{128u,72u}) for (unsigned scale:{4u,8u,16u}) for (unsigned orientation=1;orientation<=8;++orientation)
            for (unsigned phase=0;phase<4;++phase) for (unsigned bits:{12u,14u,16u})
                calibrated_preview(root,width,scale,orientation,orientation%2,orientation%3==0,phase,bits);
        invalid_calibration();
        profile_parser_tests();
        std::cout<<"PASS: independent stock RGB across all twelve CFA/depth combinations, full-frame reductions, pitched display, unaligned input, concurrent render, serial/parallel large previews, bounded native reader and corruption/EOF rejection\n";
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
