/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "vc5_encoder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define REQUIRE(test) do { if (!(test)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#test);exit(1);} } while (0)
static unsigned calls;
static size_t request_size;
static void *deny(size_t size) {++calls;request_size=size;return NULL;}
static vc5_encoder_parameters defaults(unsigned w,unsigned h,unsigned format,unsigned quality) {
    vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);
    p.input_width=w;p.input_height=h;p.input_pitch=(int)(w*2);
    p.pixel_format=(VC5_ENCODER_PIXEL_FORMAT)format;p.quality_setting=(VC5_ENCODER_QUALITY_SETTING)quality;
    return p;
}
static void rejected(vc5_encoder_parameters p,gpr_buffer input) {
    gpr_buffer out={0};p.mem_alloc=deny;calls=0;
    REQUIRE(vc5_encoder_process(&p,&input,&out,NULL)!=CODEC_ERROR_OKAY);
    REQUIRE(!calls && !out.buffer && !out.size);
}
static void invalid(void) {
    unsigned char bytes[128*96*2]={0};gpr_buffer input={bytes,sizeof(bytes)},output={0};
    vc5_encoder_parameters p=defaults(128,96,VC5_ENCODER_PIXEL_FORMAT_RGGB_14,3),q;
    unsigned i;
    const unsigned bad[]={0,1,8,16,24,32,40,46,47,49,65536,UINT_MAX};
    for (i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        q=p;q.input_width=bad[i];rejected(q,input);
        q=p;q.input_height=bad[i];rejected(q,input);
    }
    q=p;q.input_width=65534;q.input_height=65534;rejected(q,input);
    q=p;q.input_pitch=-1;rejected(q,input);q.input_pitch=0;rejected(q,input);
    q.input_pitch=255;rejected(q,input);
    q=p;q.quality_setting=(VC5_ENCODER_QUALITY_SETTING)-1;rejected(q,input);
    q.quality_setting=(VC5_ENCODER_QUALITY_SETTING)6;rejected(q,input);
    q=p;q.pixel_format=(VC5_ENCODER_PIXEL_FORMAT)99;rejected(q,input);
    input.size=sizeof(bytes)-1;rejected(p,input);input.size=2;rejected(p,input);
    input.size=0;rejected(p,input);input.size=sizeof(bytes);input.buffer=NULL;rejected(p,input);
    input.buffer=bytes;
    REQUIRE(vc5_encoder_process(NULL,&input,&output,NULL)==CODEC_ERROR_BAD_ARGUMENT);
    REQUIRE(vc5_encoder_process(&p,NULL,&output,NULL)==CODEC_ERROR_BAD_ARGUMENT);
    REQUIRE(vc5_encoder_process(&p,&input,NULL,NULL)==CODEC_ERROR_BAD_ARGUMENT);
    REQUIRE(vc5_encoder_process(&p,&input,&input,NULL)==CODEC_ERROR_BAD_ARGUMENT);
    REQUIRE(input.buffer==bytes && input.size==sizeof(bytes));
    q=p;q.mem_alloc=NULL;REQUIRE(vc5_encoder_process(&q,&input,&output,NULL)==CODEC_ERROR_BAD_ARGUMENT);
    q=p;q.mem_free=NULL;REQUIRE(vc5_encoder_process(&q,&input,&output,NULL)==CODEC_ERROR_BAD_ARGUMENT);
    /* Allocation failure proves the aligned path begins directly with codec
     * output storage, while unaligned input begins with bounded aligned scratch. */
    p.mem_alloc=deny;calls=0;
    REQUIRE(vc5_encoder_process(&p,&input,&output,NULL)==CODEC_ERROR_OUTOFMEMORY && calls==1);
    REQUIRE(request_size==sizeof(bytes)+sizeof(bytes)/2+1024*1024);
    input.buffer=bytes+1;input.size=sizeof(bytes);calls=0;
    REQUIRE(vc5_encoder_process(&p,&input,&output,NULL)==CODEC_ERROR_OUTOFMEMORY && calls==1);
    REQUIRE(request_size==sizeof(bytes)+63+sizeof(void *));
}
static unsigned bits(unsigned format) {
    const unsigned values[]={12,12,14,12,12,16,16,14,12,14,16,12,14,16};
    return values[format];
}
static gpr_buffer encode(const unsigned char *data,size_t size,unsigned width,unsigned height,
                         unsigned pitch,unsigned format,unsigned quality) {
    vc5_encoder_parameters p=defaults(width,height,format,quality);
    gpr_buffer input={(void *)data,size},output={0};p.input_pitch=(int)pitch;
    REQUIRE(vc5_encoder_process(&p,&input,&output,NULL)==CODEC_ERROR_OKAY);
    REQUIRE(output.buffer && output.size);return output;
}
static void geometry(unsigned width,unsigned height,unsigned *cases) {
    unsigned format,quality,y,x,variant;
    for (format=0;format<14;++format) {
        int packed=format==1 || format==4;
        size_t row=packed?width*3/2:width*2,pitch=(row+1)&~(size_t)1;
        size_t capacity=(height-1)*pitch+row;
        unsigned char *input=(unsigned char *)malloc(capacity);REQUIRE(input);
        for (y=0;y<height;++y) for (x=0;x<width;x+=2) {
            unsigned a=(x*123+y*97+x*y*29+(x^y)*37)&16383;
            unsigned b=((x+1)*123+y*97+(x+1)*y*29+((x+1)^y)*37)&16383;
            if (bits(format)==12) {a>>=2;b>>=2;}
            if (bits(format)==16) {a<<=2;b<<=2;}
            if (packed) {
                unsigned char *p=input+y*pitch+x*3/2;
                p[0]=(unsigned char)a;p[1]=(unsigned char)((a>>8)|((b&15)<<4));p[2]=(unsigned char)(b>>4);
            } else {
                uint16_t values[2]={(uint16_t)a,(uint16_t)b};memcpy(input+y*pitch+x*2,values,4);
            }
        }
        for (quality=0;quality<6;++quality) {
            gpr_buffer baseline=encode(input,capacity,width,height,(unsigned)pitch,format,quality);
            for (variant=0;variant<3;++variant) {
                size_t offset=variant!=1,copy_pitch=pitch+(variant!=0);
                size_t size=(height-1)*copy_pitch+row;
                unsigned char *memory=(unsigned char *)malloc(offset+size);REQUIRE(memory);
                memset(memory,0xd3,offset+size);
                for (y=0;y<height;++y) memcpy(memory+offset+y*copy_pitch,input+y*pitch,row);
                {
                    gpr_buffer actual=encode(memory+offset,size,width,height,(unsigned)copy_pitch,format,quality);
                    REQUIRE(actual.size==baseline.size && !memcmp(actual.buffer,baseline.buffer,baseline.size));
                    free(actual.buffer);++*cases;
                }
                if (offset) REQUIRE(memory[0]==0xd3);
                for (y=0;y+1<height;++y) for (x=(unsigned)row;x<copy_pitch;++x) REQUIRE(memory[offset+y*copy_pitch+x]==0xd3);
                free(memory);
            }
            free(baseline.buffer);
        }
        free(input);
    }
}
int main(void) {
    unsigned cases=0;invalid();geometry(48,48,&cases);geometry(50,50,&cases);geometry(72,96,&cases);
    printf("PASS: pre-allocation encoder rejection, scratch failure, minimum/even-tail geometry and %u unaligned/padded/odd-stride preset comparisons\n",cases);
    return 0;
}
