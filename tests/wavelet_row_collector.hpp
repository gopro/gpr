// Experimental bounded streaming inverse; full planes only for final output.
#include <algorithm>
#include <array>
#include <vector>
extern "C" CODEC_ERROR gpr_preview_inverse_row_dequantized(const PIXEL *const[3],const PIXEL *const[3],
 const PIXEL *,const PIXEL *,DIMENSION,DIMENSION,int,uint16_t,PIXEL *,PIXEL *,PIXEL *);
extern "C" CODEC_ERROR DequantizeBandRow16s(PIXEL *,int,int,PIXEL *);
struct RowChannel {
 unsigned w1,h1,w2,h2,width,height,hp_mask;
 const int16_t *cubic;
 std::vector<int16_t> upper,hp,ll,scratch_upper,scratch_lower,plane;
 QUANT quant[2][4]={{0}};unsigned prescale[2]={0,0};
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
 int16_t expand(int value)const{int magnitude=std::min(value<0?-value:value,1023);return int16_t(value<0?-cubic[magnitude]:cubic[magnitude]);}
 void drain_lower(){
  while(ready(lower_next,h1,std::min(hp_received,ll_received))){
   unsigned n=lower_next,start=n==0?0:n==h1-1?h1-3:n-1;const PIXEL *llrows[3],*lhrows[3];
   for(unsigned j=0;j<3;++j){llrows[j]=low(start+j);lhrows[j]=high(start+j,0);}
   PIXEL *even=plane.data()+size_t(2*n)*width,*odd=2*n+1<height?even+width:nullptr;
   REQUIRE(!gpr_preview_inverse_row_dequantized(llrows,lhrows,high(n,1),high(n,2),w1,width,n==0?0:n==h1-1?2:1,prescale[0],scratch_lower.data(),even,odd));
   ++lower_next;
  }
 }
 void drain_upper(){
  while(ready(upper_next,h2,upper_received)){
   unsigned n=upper_next,start=n==0?0:n==h2-1?h2-3:n-1;const PIXEL *llrows[3],*lhrows[3];
   for(unsigned j=0;j<3;++j){llrows[j]=u(start+j,0);lhrows[j]=u(start+j,1);}
   REQUIRE(2*n==ll_received&&2*n-oldest()<4);if(2*n+1<h1)REQUIRE(2*n+1-oldest()<4);
   max_ll_live=std::max(max_ll_live,std::min(2*n+2,h1)-oldest());
   REQUIRE(!gpr_preview_inverse_row_dequantized(llrows,lhrows,u(n,2),u(n,3),w2,w1,n==0?0:n==h2-1?2:1,prescale[1],scratch_upper.data(),low(2*n),2*n+1<h1?low(2*n+1):nullptr));
   ll_received=std::min(2*n+2,h1);++upper_next;drain_lower();
  }
 }
 void accept(const vc5_preview_row &row){
  REQUIRE(row.level<2&&row.width==(row.level?w2:w1)&&row.height==(row.level?h2:h1));
  unsigned received=row.level?upper_received:hp_received;REQUIRE(row.row==received);
  if(!received){prescale[row.level]=row.prescale;for(unsigned b=0;b<4;++b){REQUIRE(row.quant[b]>0&&row.quant[b]<=32767);quant[row.level][b]=QUANT(row.quant[b]);}}
  else {REQUIRE(prescale[row.level]==row.prescale);for(unsigned b=0;b<4;++b)REQUIRE(quant[row.level][b]==row.quant[b]);}
  if(!row.level){
   REQUIRE(row.row-oldest()<=hp_mask);max_hp_live=std::max(max_hp_live,row.row-oldest()+1);
   for(unsigned b=1;b<4;++b){auto *out=high(row.row,b-1);for(unsigned x=0;x<w1;++x)out[x]=expand(row.bands[b][x]);REQUIRE(!DequantizeBandRow16s(out,w1,quant[0][b],out));}
   ++hp_received;drain_lower();
  }else{
   unsigned needed=upper_next==0?0:upper_next==h2-1?h2-3:upper_next-1;REQUIRE(row.row-needed<3);
   for(unsigned b=0;b<4;++b){auto *out=u(row.row,b);for(unsigned x=0;x<w2;++x)out[x]=b?expand(row.bands[b][x]):row.bands[b][x];if(b)REQUIRE(!DequantizeBandRow16s(out,w2,quant[1][b],out));}
   ++upper_received;drain_upper();
  }
 }
 bool complete()const{return upper_received==h2&&upper_next==h2&&hp_received==h1&&ll_received==h1&&lower_next==h1;}
};
