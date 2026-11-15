/* Compare the new row inverse with the unchanged full-plane inverse. */
#include "../source/lib/vc5_decoder/headers.h"
#include <stdio.h>
extern CODEC_ERROR gpr_preview_inverse_row(const PIXEL *const[3],const PIXEL *const[3],
 const PIXEL *,const PIXEL *,DIMENSION,DIMENSION,int,uint16_t,const QUANT[4],PIXEL *,PIXEL *,PIXEL *);
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"inverse row line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
 const unsigned widths[]={3,4,7,8,17,32,65},heights[]={3,4,7,8,19};
 const QUANT quants[][4]={{1,1,1,1},{1,12,24,144},{1,1023,511,3}};
 gpr_allocator a={malloc,free};unsigned cases=0;uint32_t random=20261110;
 for(unsigned wi=0;wi<7;++wi)for(unsigned hi=0;hi<5;++hi)
 for(unsigned ox=0;ox<2;++ox)for(unsigned oy=0;oy<2;++oy)
 for(unsigned ps=0;ps<2;++ps)for(unsigned qi=0;qi<3;++qi){
  unsigned w=widths[wi],h=heights[hi],outw=2*w-ox,outh=2*h-oy;size_t size=(size_t)outw*outh;
  PIXEL *bands[4];for(unsigned b=0;b<4;++b){
   bands[b]=malloc(w*h*2);REQUIRE(bands[b]);
   for(unsigned i=0;i<w*h;++i){
    random=random*1664525u+1013904223u;
    /* LL is already expanded. Highpass inputs are entropy-decoded cubic
     * symbols, whose magnitude is at most 255, before UncompandedValue.
     * Full int16 highpass values violate that legacy inverse contract. */
    bands[b][i]=b?(PIXEL)((int)((random>>16)%511)-255):(int16_t)(random>>16);
   }
   if(b){bands[b][0]=-255;bands[b][1]=255;bands[b][2]=0;}
  }
  PIXEL *expected=malloc((size+32)*2),*actual=malloc((size+32)*2),*scratch=malloc(9*w*2);REQUIRE(expected&&actual&&scratch);
  for(unsigned i=0;i<size+32;++i)expected[i]=actual[i]=0x5678;
  QUANT q[4];memcpy(q,quants[qi],sizeof(q));
  CODEC_ERROR e=ps?InvertSpatialQuantDescale16s(&a,bands[0],w*2,bands[1],w*2,bands[2],w*2,bands[3],w*2,expected,outw*2,w,h,outw,outh,2,q):InvertSpatialQuant16s(&a,bands[0],w*2,bands[1],w*2,bands[2],w*2,bands[3],w*2,expected,outw*2,w,h,outw,outh,q);
  REQUIRE(!e);
  for(unsigned row=0;row<h;++row){unsigned start=row==0?0:row==h-1?h-3:row-1;const PIXEL *ll[3],*lh[3];
   for(unsigned j=0;j<3;++j){ll[j]=bands[0]+(start+j)*w;lh[j]=bands[1]+(start+j)*w;}
   PIXEL *even=actual+(size_t)(2*row)*outw,*odd=(2*row+1<outh)?even+outw:NULL;
   REQUIRE(!gpr_preview_inverse_row(ll,lh,bands[2]+row*w,bands[3]+row*w,w,outw,row==0?0:row==h-1?2:1,ps?2:0,q,scratch,even,odd));
  }
  REQUIRE(!memcmp(expected,actual,(size+32)*2));
  free(expected);free(actual);free(scratch);for(unsigned b=0;b<4;++b)free(bands[b]);++cases;
 }
 printf("{\"cases\":%u,\"all_inverse_samples_exact\":true,\"trailing_guards_exact\":true}\n",cases);
 return 0;
}
