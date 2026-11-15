// Public encoder/preview contracts: tests do not supply reconstruction code.
#include "vc5_encoder.h"
#include "vc5_decoder.h"
#include "gpr_preview_stream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"public stream line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static void pipeline_off(){
#ifdef _WIN32
 _putenv_s("GPR_PIPELINE_WAVELETS","");
#else
 unsetenv("GPR_PIPELINE_WAVELETS");
#endif
}
struct Rows {
 gpr_preview_stream *stream=nullptr;
 explicit Rows(unsigned w,unsigned h){REQUIRE(!gpr_preview_stream_create(w,h,128u<<20,&stream));}
 ~Rows(){gpr_preview_stream_destroy(stream);}
 static void accept(void *user,const vc5_preview_row *row){auto&s=*static_cast<Rows*>(user);REQUIRE(!gpr_preview_stream_push(s.stream,row));}
};
static void compare(const gpr_buffer &reference,Rows &rows){
 gpr_preview_wavelet_planes result={};REQUIRE(!gpr_preview_stream_finish(rows.stream,&result));
 vc5_decoder_parameters dp;vc5_decoder_parameters_set_default(&dp);dp.mem_alloc=malloc;dp.mem_free=free;dp.rgb_bits=16;dp.rgb_resolution=GPR_RGB_RESOLUTION_QUARTER;
 for(unsigned c=0;c<3;++c){gpr_rgb_buffer expected={};REQUIRE(!vc5_decoder_process_wavelet_channel(&dp,&reference,c,&expected));REQUIRE(expected.width==result.width&&expected.height==result.height&&expected.size==result.sample_count[c]*2&&!std::memcmp(expected.buffer,result.samples[c],expected.size));free(expected.buffer);}
 REQUIRE(!gpr_preview_stream_reset(rows.stream));result.samples[0]=reinterpret_cast<const int16_t*>(1);REQUIRE(gpr_preview_stream_finish(rows.stream,&result)==GPR_PREVIEW_PAYLOAD&&!result.samples[0]);
}
static void run(unsigned w,unsigned h,unsigned phase,unsigned depth,unsigned quality){
 const VC5_ENCODER_PIXEL_FORMAT formats[4][3]={
 {VC5_ENCODER_PIXEL_FORMAT_RGGB_12,VC5_ENCODER_PIXEL_FORMAT_RGGB_14,VC5_ENCODER_PIXEL_FORMAT_RGGB_16},
 {VC5_ENCODER_PIXEL_FORMAT_GBRG_12,VC5_ENCODER_PIXEL_FORMAT_GBRG_14,VC5_ENCODER_PIXEL_FORMAT_GBRG_16},
 {VC5_ENCODER_PIXEL_FORMAT_GRBG_12,VC5_ENCODER_PIXEL_FORMAT_GRBG_14,VC5_ENCODER_PIXEL_FORMAT_GRBG_16},
 {VC5_ENCODER_PIXEL_FORMAT_BGGR_12,VC5_ENCODER_PIXEL_FORMAT_BGGR_14,VC5_ENCODER_PIXEL_FORMAT_BGGR_16}};
 std::vector<uint16_t> raw(size_t(w)*h);uint32_t random=71;unsigned bits=12+2*depth;
 for(auto&v:raw){random=random*1664525u+1013904223u;v=uint16_t((random>>16)&((1u<<bits)-1));}
 vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);p.input_width=w;p.input_height=h;p.input_pitch=w*2;p.pixel_format=formats[phase][depth];p.quality_setting=(VC5_ENCODER_QUALITY_SETTING)quality;
 gpr_buffer input={raw.data(),raw.size()*2},reference={};pipeline_off();REQUIRE(!vc5_encoder_process(&p,&input,&reference,nullptr));Rows rows(w,h);
 // Same object is reused; reset must remove all prior frame state.
 for(unsigned repeat=0;repeat<2;++repeat){gpr_buffer output={};REQUIRE(!vc5_encoder_process_with_preview_rows(&p,&input,&output,nullptr,Rows::accept,&rows));REQUIRE(output.size==reference.size&&!std::memcmp(output.buffer,reference.buffer,output.size));compare(output,rows);free(output.buffer);}
 free(reference.buffer);
}
static void packed(unsigned phase){
 unsigned w=50,h=50;size_t active=w*3/2,pitch=active+2;std::vector<unsigned char> raw(1+(h-1)*pitch+active,0xcd);
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;x+=2){unsigned a=(x*97+y*83)&4095,b=(x*47+y*29)&4095;auto*p=raw.data()+1+y*pitch+x*3/2;p[0]=static_cast<unsigned char>(a);p[1]=static_cast<unsigned char>((a>>8)|((b&15)<<4));p[2]=static_cast<unsigned char>(b>>4);}
 vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);p.input_width=w;p.input_height=h;p.input_pitch=int(pitch);p.pixel_format=phase?VC5_ENCODER_PIXEL_FORMAT_GBRG_12P:VC5_ENCODER_PIXEL_FORMAT_RGGB_12P;
 gpr_buffer input={raw.data()+1,raw.size()-1},reference={},output={};Rows rows(w,h);REQUIRE(!vc5_encoder_process(&p,&input,&reference,nullptr));REQUIRE(!vc5_encoder_process_with_preview_rows(&p,&input,&output,nullptr,Rows::accept,&rows));REQUIRE(output.size==reference.size&&!memcmp(output.buffer,reference.buffer,output.size));compare(output,rows);free(reference.buffer);free(output.buffer);
}
static void changing_quality(){
 const unsigned w=130,h=98;std::vector<uint16_t> raw(w*h);uint32_t random=83;
 for(auto&v:raw){random=random*1664525u+1013904223u;v=uint16_t((random>>16)&16383);}
 vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);p.input_width=w;p.input_height=h;p.input_pitch=w*2;p.pixel_format=VC5_ENCODER_PIXEL_FORMAT_RGGB_14;
 gpr_buffer input={raw.data(),raw.size()*2};Rows rows(w,h);const unsigned qualities[]={0,5,3,1,4,2,5,0};
 for(unsigned q:qualities){p.quality_setting=(VC5_ENCODER_QUALITY_SETTING)q;gpr_buffer reference={},output={};REQUIRE(!vc5_encoder_process(&p,&input,&reference,nullptr));REQUIRE(!vc5_encoder_process_with_preview_rows(&p,&input,&output,nullptr,Rows::accept,&rows));REQUIRE(output.size==reference.size&&!memcmp(output.buffer,reference.buffer,output.size));compare(output,rows);free(reference.buffer);free(output.buffer);}
}
static unsigned allocations;
static void*deny(size_t){++allocations;return nullptr;}
static void invalid(){
 gpr_preview_stream *s=reinterpret_cast<gpr_preview_stream*>(1);REQUIRE(gpr_preview_stream_create(47,48,1u<<20,&s)==GPR_PREVIEW_INVALID&&!s);REQUIRE(gpr_preview_stream_create(48,48,1,&s)==GPR_PREVIEW_CAPACITY&&!s);REQUIRE(!gpr_preview_stream_create(48,48,1u<<20,&s));REQUIRE(gpr_preview_stream_buffer_bytes(s)>0&&gpr_preview_stream_buffer_bytes(s)<(1u<<20));
 const size_t exact_bytes=gpr_preview_stream_buffer_bytes(s);gpr_preview_stream *bounded=nullptr;REQUIRE(gpr_preview_stream_create(48,48,exact_bytes-1,&bounded)==GPR_PREVIEW_CAPACITY&&!bounded);REQUIRE(!gpr_preview_stream_create(48,48,exact_bytes,&bounded));REQUIRE(gpr_preview_stream_buffer_bytes(bounded)==exact_bytes);gpr_preview_stream_destroy(bounded);
 gpr_preview_wavelet_planes out={};REQUIRE(gpr_preview_stream_finish(s,&out)==GPR_PREVIEW_PAYLOAD&&!out.samples[0]);REQUIRE(gpr_preview_stream_push(s,nullptr)==GPR_PREVIEW_INVALID);REQUIRE(gpr_preview_stream_finish(s,&out)==GPR_PREVIEW_INVALID&&!out.samples[0]);REQUIRE(!gpr_preview_stream_reset(s));
 int16_t data[6]={};vc5_preview_row r={};r.width=r.height=6;for(unsigned b=0;b<4;++b){r.bands[b]=data;r.band_samples[b]=6;r.quant[b]=1;}
 r.band_samples[1]=5;REQUIRE(gpr_preview_stream_push(s,&r)==GPR_PREVIEW_INVALID);REQUIRE(!gpr_preview_stream_reset(s));r.band_samples[1]=6;
 r.prescale=1;REQUIRE(gpr_preview_stream_push(s,&r)==GPR_PREVIEW_INVALID);REQUIRE(!gpr_preview_stream_reset(s));r.prescale=0;
 REQUIRE(!gpr_preview_stream_push(s,&r));REQUIRE(gpr_preview_stream_push(s,&r)==GPR_PREVIEW_INVALID);REQUIRE(!gpr_preview_stream_reset(s));
 r.quant[1]=32768;REQUIRE(gpr_preview_stream_push(s,&r)==GPR_PREVIEW_INVALID);REQUIRE(gpr_preview_stream_finish(s,&out)==GPR_PREVIEW_INVALID&&!out.samples[0]);REQUIRE(!gpr_preview_stream_reset(s));r.quant[1]=1;r.channel=3;REQUIRE(gpr_preview_stream_push(s,&r)==GPR_PREVIEW_INVALID);gpr_preview_stream_destroy(s);
 vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);p.input_width=p.input_height=48;p.input_pitch=96;p.mem_alloc=deny;p.mem_free=free;std::vector<uint16_t> raw(48*48);gpr_buffer input={raw.data(),raw.size()*2-1},output={};allocations=0;REQUIRE(vc5_encoder_process_with_preview_rows(&p,&input,&output,nullptr,nullptr,nullptr)==CODEC_ERROR_BAD_ARGUMENT&&!allocations);input.size++;REQUIRE(vc5_encoder_process_with_preview_rows(&p,&input,&output,nullptr,nullptr,nullptr)==CODEC_ERROR_OUTOFMEMORY&&allocations);
}
int main(){invalid();changing_quality();const unsigned widths[5]={48,50,130,192,206},heights[5]={48,50,98,128,150};unsigned cases=0;
 for(unsigned shape=0;shape<5;++shape)for(unsigned phase=0;phase<4;++phase)for(unsigned depth=0;depth<3;++depth)for(unsigned q=0;q<6;++q){run(widths[shape],heights[shape],phase,depth,q);++cases;}
 for(unsigned h=48;h<=320;h+=2){run(130,h,0,1,3);++cases;}packed(0);packed(1);
 std::printf("{\"cases\":%u,\"reused_frames_per_case\":2,\"packed_unaligned_cases\":2,\"public_payloads_exact\":true,\"public_planes_exact_independent_decode\":true,\"invalid_inputs_and_reset\":true,\"changing_presets_reused_stream\":8,\"exact_capacity_boundary\":true}\n",cases);
}
