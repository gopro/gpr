// Experimental full coefficient-view equivalence, including unconsumed bands.
#include "vc5_encoder.h"
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"contract line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Snapshot {std::array<std::vector<int16_t>,24> bands;vc5_preview_coefficients layout={};unsigned calls=0;bool compare=false;};
static void observe(void *user,const vc5_preview_coefficients *v){
 auto &s=*static_cast<Snapshot*>(user);++s.calls;
 if(!s.compare)s.layout=*v;
 REQUIRE(s.layout.sensor_width==v->sensor_width&&s.layout.sensor_height==v->sensor_height);
 for(unsigned c=0;c<3;++c)for(unsigned l=0;l<2;++l){
  const auto &a=s.layout.channels[c][l],&b=v->channels[c][l];
  REQUIRE(a.width==b.width&&a.height==b.height&&a.pitch_samples==b.pitch_samples&&a.prescale==b.prescale);
  for(unsigned band=0;band<4;++band){
   REQUIRE(a.quant[band]==b.quant[band]&&a.band_samples[band]==b.band_samples[band]);auto &copy=s.bands[c*8+l*4+band];
   if(!s.compare)copy.resize(size_t(b.width)*b.height);
   for(unsigned y=0;y<b.height;++y){auto *row=b.bands[band]+size_t(y)*b.pitch_samples;
    if(!s.compare)std::memcpy(copy.data()+size_t(y)*b.width,row,b.width*2);
    else REQUIRE(!std::memcmp(copy.data()+size_t(y)*b.width,row,b.width*2));
   }
  }
 }
}
int main(){
 const unsigned bits[12]={12,14,16,12,14,16,12,14,16,12,14,16};
 const VC5_ENCODER_PIXEL_FORMAT formats[12]={
 VC5_ENCODER_PIXEL_FORMAT_RGGB_12,VC5_ENCODER_PIXEL_FORMAT_RGGB_14,VC5_ENCODER_PIXEL_FORMAT_RGGB_16,
 VC5_ENCODER_PIXEL_FORMAT_GBRG_12,VC5_ENCODER_PIXEL_FORMAT_GBRG_14,VC5_ENCODER_PIXEL_FORMAT_GBRG_16,
 VC5_ENCODER_PIXEL_FORMAT_GRBG_12,VC5_ENCODER_PIXEL_FORMAT_GRBG_14,VC5_ENCODER_PIXEL_FORMAT_GRBG_16,
 VC5_ENCODER_PIXEL_FORMAT_BGGR_12,VC5_ENCODER_PIXEL_FORMAT_BGGR_14,VC5_ENCODER_PIXEL_FORMAT_BGGR_16};unsigned cases=0;
 for(unsigned shape=0;shape<5;++shape)for(unsigned f=0;f<12;++f)for(unsigned q=0;q<6;++q){
  const unsigned widths[5]={48,50,130,192,206},heights[5]={48,50,98,128,150};unsigned w=widths[shape],h=heights[shape];std::vector<uint16_t> raw(size_t(w)*h);uint32_t rng=37;
  for(auto &x:raw){rng=rng*1664525u+1013904223u;x=uint16_t((rng>>16)&((1u<<bits[f])-1));}
  vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);p.input_width=w;p.input_height=h;p.input_pitch=w*2;p.pixel_format=formats[f];p.quality_setting=(VC5_ENCODER_QUALITY_SETTING)q;
  gpr_buffer input={raw.data(),raw.size()*2},a={},b={};Snapshot s;

#ifdef _WIN32
  _putenv_s("GPR_PIPELINE_WAVELETS","");
#else
  unsetenv("GPR_PIPELINE_WAVELETS");
#endif
REQUIRE(!vc5_encoder_process_with_preview(&p,&input,&a,nullptr,observe,&s));
  s.compare=true;
#ifdef _WIN32
  _putenv_s("GPR_PIPELINE_WAVELETS","1");
#else
  setenv("GPR_PIPELINE_WAVELETS","1",1);
#endif
REQUIRE(!vc5_encoder_process_with_preview(&p,&input,&b,nullptr,observe,&s));
  REQUIRE(s.calls==2&&a.size==b.size&&!std::memcmp(a.buffer,b.buffer,a.size));std::free(a.buffer);std::free(b.buffer);++cases;
 }
 std::printf("{\"cases\":%u,\"payloads_exact\":true,\"all_24_bands_exact\":true,\"view_metadata_exact\":true}\n",cases);
}
