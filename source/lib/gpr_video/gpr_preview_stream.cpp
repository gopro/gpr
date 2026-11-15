/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_preview_stream.h"
#include "gpr_preview_dequantize.h"
#include "vc5_decoder.h"
#include <atomic>
#include <climits>
#include <new>
extern "C" CODEC_ERROR ComputeCubicTable(int16_t *,int,int16_t);
extern "C" int32_t UncompandedValue(int32_t);
// Bounded channel state; full planes exist only for the final output.
#include <algorithm>
#include <array>
#include <vector>
extern "C" CODEC_ERROR gpr_preview_inverse_row_dequantized(const int16_t *const[3],const int16_t *const[3],
 const int16_t *,const int16_t *,uint_least16_t,uint_least16_t,int,uint16_t,int16_t *,int16_t *,int16_t *);
struct InvalidRows {};
static void require_rows(bool okay){if(!okay)throw InvalidRows{};}
struct RowChannel {
 unsigned w1,h1,w2,h2,width,height,hp_mask;
 const int16_t *cubic;
 std::array<int16_t,2*3*2047> dequantized{};
 int cached_quant[2][3]={{0}};
 std::vector<int16_t> upper,hp,ll,scratch_upper,scratch_lower,plane;
 int quant[2][4]={{0}};unsigned prescale[2]={0,0};
 unsigned upper_received=0,upper_next=0,hp_received=0,ll_received=0,lower_next=0;
 unsigned max_hp_live=0,max_ll_live=0;
 RowChannel(unsigned w,unsigned h,const int16_t *table):
  w1((w+7)/8),h1((h+7)/8),w2((w+15)/16),h2((h+15)/16),width((w+3)/4),height((h+3)/4),hp_mask((h1&1)?15:7),cubic(table),
  upper(3*4*w2),hp((hp_mask+1)*3*w1),ll(4*w1),scratch_upper(4*w2),scratch_lower(4*w1),plane(size_t(width)*height){}
 void reset(){upper_received=upper_next=hp_received=ll_received=lower_next=max_hp_live=max_ll_live=0;}
 static bool ready(unsigned next,unsigned size,unsigned received){return next<size&&received>=(next==0?3:next==size-1?size:next+2);}
 unsigned oldest()const{return lower_next>=h1?h1:lower_next==0?0:lower_next==h1-1?h1-3:lower_next-1;}
 int16_t *u(unsigned row,unsigned band){return upper.data()+((row%3)*4+band)*w2;}
 int16_t *high(unsigned row,unsigned band){return hp.data()+((row&hp_mask)*3+band)*w1;}
 int16_t *low(unsigned row){return ll.data()+(row%4)*w1;}
 const int16_t *table(unsigned level,unsigned band){
  int q=quant[level][band];auto *out=dequantized.data()+(level*3+band-1)*2047;
  if(cached_quant[level][band-1]!=q){
   for(int value=-1023;value<=1023;++value)out[value+1023]=gpr_preview_dequantize_sample(cubic,int16_t(value),uint32_t(q));
   cached_quant[level][band-1]=q;
  }
  return out;
 }
 void dequantize_row(unsigned level,unsigned band,const int16_t *input,int16_t *out,unsigned width){
  const int16_t *lookup=table(level,band);
  for(unsigned x=0;x<width;++x)out[x]=gpr_preview_quantized_lookup_sample(lookup,input[x]);
 }
 void drain_lower(){
  while(ready(lower_next,h1,std::min(hp_received,ll_received))){
   unsigned n=lower_next,start=n==0?0:n==h1-1?h1-3:n-1;const int16_t *llrows[3],*lhrows[3];
   for(unsigned j=0;j<3;++j){llrows[j]=low(start+j);lhrows[j]=high(start+j,0);}
   int16_t *even=plane.data()+size_t(2*n)*width,*odd=2*n+1<height?even+width:nullptr;
   require_rows(!gpr_preview_inverse_row_dequantized(llrows,lhrows,high(n,1),high(n,2),w1,width,n==0?0:n==h1-1?2:1,prescale[0],scratch_lower.data(),even,odd));
   ++lower_next;
  }
 }
 void drain_upper(){
  while(ready(upper_next,h2,upper_received)){
   unsigned n=upper_next,start=n==0?0:n==h2-1?h2-3:n-1;const int16_t *llrows[3],*lhrows[3];
   for(unsigned j=0;j<3;++j){llrows[j]=u(start+j,0);lhrows[j]=u(start+j,1);}
   require_rows(2*n==ll_received&&2*n-oldest()<4);if(2*n+1<h1)require_rows(2*n+1-oldest()<4);
   max_ll_live=std::max(max_ll_live,std::min(2*n+2,h1)-oldest());
   require_rows(!gpr_preview_inverse_row_dequantized(llrows,lhrows,u(n,2),u(n,3),w2,w1,n==0?0:n==h2-1?2:1,prescale[1],scratch_upper.data(),low(2*n),2*n+1<h1?low(2*n+1):nullptr));
   ll_received=std::min(2*n+2,h1);++upper_next;drain_lower();
  }
 }
 void accept(const vc5_preview_row &row){
  require_rows((row.prescale==0||row.prescale==2)&&row.level<2&&row.width==(row.level?w2:w1)&&row.height==(row.level?h2:h1));
  for(unsigned b=row.level?0:1;b<4;++b)require_rows(row.band_samples[b]>=row.width&&row.bands[b]&&!(reinterpret_cast<uintptr_t>(row.bands[b])&1));
  unsigned received=row.level?upper_received:hp_received;require_rows(row.row==received);
  if(!received){prescale[row.level]=row.prescale;for(unsigned b=0;b<4;++b){require_rows(row.quant[b]>0&&row.quant[b]<=32767);quant[row.level][b]=int(row.quant[b]);}}
  else {require_rows(prescale[row.level]==row.prescale);for(unsigned b=0;b<4;++b)require_rows(quant[row.level][b]==row.quant[b]);}
  if(!row.level){
   require_rows(row.row-oldest()<=hp_mask);max_hp_live=std::max(max_hp_live,row.row-oldest()+1);
   for(unsigned b=1;b<4;++b){auto *out=high(row.row,b-1);dequantize_row(0,b,row.bands[b],out,w1);}
   ++hp_received;drain_lower();
  }else{
   unsigned needed=upper_next==0?0:upper_next==h2-1?h2-3:upper_next-1;require_rows(row.row-needed<3);
   for(unsigned b=0;b<4;++b){auto *out=u(row.row,b);if(b)dequantize_row(1,b,row.bands[b],out,w2);else std::copy(row.bands[b],row.bands[b]+w2,out);}
   ++upper_received;drain_upper();
  }
 }
 bool complete()const{return upper_received==h2&&upper_next==h2&&hp_received==h1&&ll_received==h1&&lower_next==h1;}
};

struct gpr_preview_stream {
 std::array<int16_t,1025> cubic;
 std::array<RowChannel,3> channels;
 std::atomic<int> error{0};
 size_t buffer_bytes;
 gpr_preview_stream(unsigned w,unsigned h,size_t bytes):channels{{RowChannel(w,h,cubic.data()),RowChannel(w,h,cubic.data()),RowChannel(w,h,cubic.data())}},buffer_bytes(bytes){
  require_rows(!ComputeCubicTable(cubic.data(),1025,255));
  // Prepare the stock inverse-companding map once for signed quantizer tables.
  // Share this map read-only across channels; each channel owns its signed
  // tables and refreshes them only when the corresponding quantizer changes.
  for(auto &value:cubic){int32_t expanded=UncompandedValue(value);require_rows(expanded>=0&&expanded<=1023);value=int16_t(expanded);}
 }
};
extern "C" int gpr_preview_stream_create(uint32_t w,uint32_t h,size_t limit,gpr_preview_stream **out){
 if(!out)return GPR_PREVIEW_INVALID;*out=nullptr;
 if(w<48||h<48||w>UINT16_MAX||h>UINT16_MAX||(w&1)||(h&1)||uint64_t(w)*h>INT_MAX/2||!limit||limit>size_t(128)*1024*1024)return GPR_PREVIEW_INVALID;
 const uint64_t w1=(w+7)/8,h1=(h+7)/8,w2=(w+15)/16,pw=(w+3)/4,ph=(h+3)/4;
 const uint64_t hp_rows=(h1&1)?16:8;
 const uint64_t bytes=3*(3*4*w2+hp_rows*3*w1+4*w1+4*w2+4*w1+pw*ph+2*3*2047)*2+1025*2;
 if(bytes>limit)return GPR_PREVIEW_CAPACITY;
 try{*out=new gpr_preview_stream(w,h,size_t(bytes));return 0;}catch(const std::bad_alloc&){return GPR_PREVIEW_NOMEM;}catch(...){return GPR_PREVIEW_INVALID;}
}
extern "C" int gpr_preview_stream_push(gpr_preview_stream *s,const vc5_preview_row *row){
 if(!s)return GPR_PREVIEW_INVALID;
 int error=s->error.load(std::memory_order_acquire);if(error)return error;
 if(!row||row->channel>=3){s->error.store(GPR_PREVIEW_INVALID,std::memory_order_release);return GPR_PREVIEW_INVALID;}
 try{s->channels[row->channel].accept(*row);return s->error.load(std::memory_order_acquire);}
 catch(...){s->error.store(GPR_PREVIEW_INVALID,std::memory_order_release);return GPR_PREVIEW_INVALID;}
}
extern "C" int gpr_preview_stream_finish(gpr_preview_stream *s,gpr_preview_wavelet_planes *out){
 if(!out)return GPR_PREVIEW_INVALID;*out={};if(!s)return GPR_PREVIEW_INVALID;
 int error=s->error.load(std::memory_order_acquire);if(error)return error;
 for(const auto&c:s->channels)if(!c.complete())return GPR_PREVIEW_PAYLOAD;
 out->width=s->channels[0].width;out->height=s->channels[0].height;
 for(unsigned c=0;c<3;++c){out->samples[c]=s->channels[c].plane.data();out->sample_count[c]=s->channels[c].plane.size();}
 return 0;
}
extern "C" int gpr_preview_stream_reset(gpr_preview_stream *s){if(!s)return GPR_PREVIEW_INVALID;for(auto&c:s->channels)c.reset();s->error.store(0,std::memory_order_release);return 0;}
extern "C" void gpr_preview_stream_destroy(gpr_preview_stream *s){delete s;}

extern "C" size_t gpr_preview_stream_buffer_bytes(const gpr_preview_stream *s){return s?s->buffer_bytes:0;}
