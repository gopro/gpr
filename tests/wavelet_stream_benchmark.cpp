#include "../source/lib/vc5_encoder/headers.h"
#include "vc5_encoder.h"
#include "vc5_decoder.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <vector>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"stream benchmark line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#include "wavelet_row_collector.hpp"
struct Arena{std::vector<unsigned char>bytes;size_t used=0;explicit Arena(size_t n):bytes(n,0){}void*alloc(size_t n){if(n>SIZE_MAX-63)return nullptr;size_t count=(n+63)&~size_t(63);if(count>bytes.size()-used)return nullptr;void*p=bytes.data()+used;used+=count;return p;}};
static thread_local Arena *current=nullptr;static void*allocate(size_t n){return current?current->alloc(n):nullptr;}static void release(void*){}
struct Legacy{
 std::array<Arena,3>scratch{{Arena(1024u*768u*16u+(1u<<20)),Arena(1024u*768u*16u+(1u<<20)),Arena(1024u*768u*16u+(1u<<20))}};
 gpr_rgb_buffer planes[3]={{}};int errors[3]={};unsigned calls=0;
 void one(unsigned c,const vc5_preview_coefficients*v){Arena*previous=current;current=&scratch[c];scratch[c].used=0;errors[c]=vc5_decoder_reconstruct_preview_channel(v,c,allocate,release,&planes[c]);current=previous;}
 static void accept(void*user,const vc5_preview_coefficients*v){auto&s=*static_cast<Legacy*>(user);++s.calls;auto a=std::async(std::launch::async,[&]{s.one(1,v);});auto b=std::async(std::launch::async,[&]{s.one(2,v);});s.one(0,v);a.get();b.get();}
};
struct Rows{
 std::array<int16_t,1025>cubic;std::array<RowChannel,3>channels;
 Rows():channels{{RowChannel(4096,3072,cubic.data()),RowChannel(4096,3072,cubic.data()),RowChannel(4096,3072,cubic.data())}}{REQUIRE(!ComputeCubicTable(cubic.data(),1025,255));}
 static void accept(void*user,const vc5_preview_row*r){REQUIRE(r&&r->channel<3);static_cast<Rows*>(user)->channels[r->channel].accept(*r);}
 void reset(){for(auto&c:channels)c.reset();}
};
static std::vector<unsigned char>read(const char*n){std::ifstream f(n,std::ios::binary);return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)),{});}
int main(int argc,char**argv){
 if(argc!=3)return 2;auto raw=read(argv[1]),ref=read(argv[2]);REQUIRE(raw.size()==4096u*3072u*2u&&!ref.empty());
 std::array<std::vector<unsigned char>,3>expected;gpr_buffer reference={ref.data(),ref.size()};vc5_decoder_parameters dp;vc5_decoder_parameters_set_default(&dp);dp.mem_alloc=malloc;dp.mem_free=free;dp.rgb_bits=16;dp.rgb_resolution=GPR_RGB_RESOLUTION_QUARTER;
 for(unsigned c=0;c<3;++c){gpr_rgb_buffer out={};REQUIRE(!vc5_decoder_process_wavelet_channel(&dp,&reference,c,&out));expected[c].assign((unsigned char*)out.buffer,(unsigned char*)out.buffer+out.size);free(out.buffer);}
 Arena encoder(192u<<20);Legacy legacy;Rows rows;vc5_encoder_parameters ep;vc5_encoder_parameters_set_default(&ep);ep.input_width=4096;ep.input_height=3072;ep.input_pitch=8192;ep.pixel_format=VC5_ENCODER_PIXEL_FORMAT_RGGB_14;ep.quality_setting=VC5_ENCODER_QUALITY_SETTING_FS1;ep.mem_alloc=allocate;ep.mem_free=release;gpr_buffer input={raw.data(),raw.size()};
 const unsigned modes[16]={0,1,2,3,3,2,1,0,2,0,3,1,1,3,0,2};const char*names[4]={"none","retained-inverse","pipelined-retained-inverse","streamed-inverse"};
 for(unsigned block=0;block<16;++block)for(unsigned iteration=0;iteration<16;++iteration){unsigned mode=modes[block];if(mode>=2)setenv("GPR_PIPELINE_WAVELETS","1",1);else unsetenv("GPR_PIPELINE_WAVELETS");encoder.used=0;current=&encoder;legacy.calls=0;rows.reset();for(unsigned c=0;c<3;++c){legacy.planes[c]={};legacy.errors[c]=0;}gpr_buffer out={};CODEC_ERROR error;
  auto begin=std::chrono::steady_clock::now();
  if(mode==3){
   ENCODER_PARAMETERS p;InitEncoderParameters(&p);p.encoded.format=IMAGE_FORMAT_RAW;p.allocator.Alloc=allocate;p.allocator.Free=release;const QUANT q[10]={1,24,24,12,24,24,12,96,96,144};memcpy(p.quant_table,q,sizeof(q));p.input.width=4096;p.input.height=3072;p.input.format=PIXEL_FORMAT_RAW_RGGB_14;p.preview_row_observer=Rows::accept;p.preview_row_user=&rows;
   IMAGE image={};image.buffer=raw.data();image.width=4096;image.height=3072;image.pitch=8192;image.size=raw.size();image.format=p.input.format;const size_t capacity=raw.size()+raw.size()/2+(1<<20);void*bytes=allocate(capacity);REQUIRE(bytes);STREAM stream;REQUIRE(!CreateStreamBuffer(&stream,bytes,capacity));error=EncodeImage(&image,&stream,nullptr,&p);out.buffer=bytes;out.size=stream.byte_count;
  }else error=vc5_encoder_process_with_preview(&ep,&input,&out,nullptr,mode?Legacy::accept:nullptr,&legacy);
  double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();REQUIRE(!error&&out.size==ref.size()&&!memcmp(out.buffer,ref.data(),ref.size()));unsigned max_hp=0,max_ll=0;
  if(mode==1||mode==2){REQUIRE(legacy.calls==1);for(unsigned c=0;c<3;++c)REQUIRE(!legacy.errors[c]&&legacy.planes[c].size==expected[c].size()&&!memcmp(legacy.planes[c].buffer,expected[c].data(),expected[c].size()));}
  if(mode==3){for(unsigned c=0;c<3;++c){auto&s=rows.channels[c];REQUIRE(s.complete()&&s.plane.size()*2==expected[c].size()&&!memcmp(s.plane.data(),expected[c].data(),expected[c].size()));max_hp=std::max(max_hp,s.max_hp_live);max_ll=std::max(max_ll,s.max_ll_live);}}
  std::printf("{\"block\":%u,\"iteration\":%u,\"warmup\":%s,\"mode\":\"%s\",\"wall_s\":%.9f,\"encoder_bytes\":%zu,\"payload_exact_independent_stock\":true,\"planes_exact_independent_decode\":%s,\"max_hp_live\":%u,\"max_ll_live\":%u}\n",block,iteration,iteration?"false":"true",names[mode],seconds,encoder.used,mode?"true":"null",max_hp,max_ll);std::fflush(stdout);
 }
}
