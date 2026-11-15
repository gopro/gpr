/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/* Internal architecture proof; no capture/public encoder API is changed. */
#include "../source/lib/vc5_encoder/headers.h"
#include "vc5_decoder.h"
#include <stdio.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"preview proof line %d: %s\n",__LINE__,#x);exit(1); } } while (0)
extern CODEC_ERROR TransformInverseSpatialQuantLowpass(gpr_allocator *,WAVELET *,WAVELET *,uint16_t);
typedef struct { gpr_allocator allocator; PIXEL *planes[3]; unsigned width,height; } PROBE;
static void observe(void *user,const TRANSFORM *transforms) {
    PROBE *probe=user;
    int16_t cubic[1025];
    REQUIRE(ComputeCubicTable(cubic,1025,255)==CODEC_ERROR_OKAY);
    for (unsigned channel=0;channel<3;++channel) {
        WAVELET *levels[3];const TRANSFORM *transform=&transforms[channel];
        for (unsigned stage=0;stage<3;++stage) {
            const WAVELET *source=transform->wavelet[stage];
            levels[stage]=CreateWavelet(&probe->allocator,source->width,source->height);
            REQUIRE(levels[stage]);
            memcpy(levels[stage]->quant,source->quant,sizeof(source->quant));
            if (!stage) continue;
            for (unsigned band=stage==2 ? 0:1;band<4;++band)
                for (unsigned y=0;y<source->height;++y) {
                    const PIXEL *in=(const PIXEL *)((const uint8_t *)source->data[band]+y*source->pitch);
                    PIXEL *out=(PIXEL *)((uint8_t *)levels[stage]->data[band]+y*levels[stage]->pitch);
                    for (unsigned x=0;x<source->width;++x) {
                        int value=in[x],magnitude=value<0 ? -value:value;
                        if (band) { if (magnitude>1023) magnitude=1023;value=cubic[magnitude]*(value<0 ? -1:1); }
                        out[x]=(PIXEL)value;
                    }
                }
        }
        REQUIRE(TransformInverseSpatialQuantLowpass(&probe->allocator,levels[2],levels[1],transform->prescale[2])==CODEC_ERROR_OKAY);
        REQUIRE(TransformInverseSpatialQuantLowpass(&probe->allocator,levels[1],levels[0],transform->prescale[1])==CODEC_ERROR_OKAY);
        probe->width=levels[0]->width;probe->height=levels[0]->height;
        probe->planes[channel]=malloc((size_t)probe->width*probe->height*sizeof(PIXEL));REQUIRE(probe->planes[channel]);
        for (unsigned y=0;y<probe->height;++y)
            memcpy(probe->planes[channel]+y*probe->width,(uint8_t *)levels[0]->data[0]+y*levels[0]->pitch,probe->width*sizeof(PIXEL));
        for (unsigned stage=0;stage<3;++stage) DeleteWavelet(&probe->allocator,levels[stage]);
    }
}
typedef struct {PROBE *reference;unsigned calls;} CALLBACK_CONTEXT;
static unsigned allocation_attempts;
static void *deny_allocation(size_t bytes) {(void)bytes;++allocation_attempts;return NULL;}
static void public_observe(void *user,const vc5_preview_coefficients *input) {
    CALLBACK_CONTEXT *context=user;gpr_rgb_buffer planes[3]={{0}};
    ++context->calls;
    vc5_preview_coefficients bad=*input;
    --bad.channels[2][1].band_samples[3];
    REQUIRE(vc5_decoder_reconstruct_preview(&bad,malloc,free,planes)==CODEC_ERROR_BAD_ARGUMENT);
    REQUIRE(!planes[0].buffer && !planes[1].buffer && !planes[2].buffer);
    allocation_attempts=0;
    REQUIRE(vc5_decoder_reconstruct_preview(input,deny_allocation,free,planes)==CODEC_ERROR_OUTOFMEMORY);
    REQUIRE(allocation_attempts && !planes[0].buffer && !planes[1].buffer && !planes[2].buffer);
    REQUIRE(vc5_decoder_reconstruct_preview(input,malloc,free,planes)==CODEC_ERROR_OKAY);
    for (unsigned channel=0;channel<3;++channel) {
        gpr_rgb_buffer one={0};
        REQUIRE(vc5_decoder_reconstruct_preview_channel(input,channel,malloc,free,&one)==CODEC_ERROR_OKAY);
        REQUIRE(one.size==planes[channel].size && !memcmp(one.buffer,planes[channel].buffer,one.size));free(one.buffer);
        REQUIRE(planes[channel].width==context->reference->width && planes[channel].height==context->reference->height);
        REQUIRE(!memcmp(planes[channel].buffer,context->reference->planes[channel],planes[channel].size));
        free(planes[channel].buffer);
    }
}
static void run_case(unsigned width,unsigned height,unsigned bits,PIXEL_FORMAT format,
    VC5_ENCODER_PIXEL_FORMAT public_format,const char *path) {
    const size_t raw_size=(size_t)width*height*2,capacity=raw_size+raw_size/2+1024*1024;
    uint16_t *raw=malloc(raw_size);uint32_t random=17;REQUIRE(raw);
    for (unsigned i=0;i<width*height;++i) {random=random*1664525u+1013904223u;raw[i]=(uint16_t)((random>>16)&((1u<<bits)-1));}
    if (path) {
        FILE *file=fopen(path,"rb");REQUIRE(file);
        REQUIRE(fread(raw,1,raw_size,file)==raw_size && fgetc(file)==EOF && !ferror(file));REQUIRE(!fclose(file));
    }
    const QUANT qualities[6][10]={{1,24,24,12,64,64,48,512,512,768},{1,24,24,12,48,48,32,256,256,384},
        {1,24,24,12,32,32,24,128,128,192},{1,24,24,12,24,24,12,96,96,144},
        {1,24,24,12,24,24,12,64,64,96},{1,24,24,12,24,24,12,32,32,48}};
    for (unsigned quality=0;quality<6;++quality) {
        ENCODER_PARAMETERS p;InitEncoderParameters(&p);p.encoded.format=IMAGE_FORMAT_RAW;
        p.allocator.Alloc=malloc;p.allocator.Free=free;memcpy(p.quant_table,qualities[quality],sizeof(p.quant_table));
        PROBE probe={0};probe.allocator=p.allocator;p.preview_observer=observe;p.preview_user=&probe;
        IMAGE image={0};image.buffer=raw;image.width=width;image.height=height;image.pitch=width*2;
        image.size=raw_size;image.format=format;
        p.input.width=width;p.input.height=height;p.input.format=image.format;
        uint8_t *bytes=malloc(capacity);REQUIRE(bytes);STREAM stream;
        REQUIRE(CreateStreamBuffer(&stream,bytes,capacity)==CODEC_ERROR_OKAY);
        REQUIRE(EncodeImage(&image,&stream,NULL,&p)==CODEC_ERROR_OKAY);
        vc5_encoder_parameters ep;vc5_encoder_parameters_set_default(&ep);
        ep.input_width=width;ep.input_height=height;ep.input_pitch=width*2;ep.pixel_format=public_format;
        ep.quality_setting=(VC5_ENCODER_QUALITY_SETTING)quality;
        gpr_buffer input={raw,raw_size},stock={0};
        REQUIRE(vc5_encoder_process(&ep,&input,&stock,NULL)==CODEC_ERROR_OKAY);
        REQUIRE(stock.size==stream.byte_count && !memcmp(stock.buffer,bytes,stock.size));
        CALLBACK_CONTEXT callback={&probe,0};gpr_buffer observed={0};
        REQUIRE(vc5_encoder_process_with_preview(&ep,&input,&observed,NULL,public_observe,&callback)==CODEC_ERROR_OKAY);
        REQUIRE(callback.calls==1 && observed.size==stock.size && !memcmp(observed.buffer,stock.buffer,stock.size));
        free(observed.buffer);
        vc5_decoder_parameters dp;vc5_decoder_parameters_set_default(&dp);dp.mem_alloc=malloc;dp.mem_free=free;
        dp.rgb_bits=16;dp.rgb_resolution=GPR_RGB_RESOLUTION_QUARTER;
        for (unsigned channel=0;channel<3;++channel) {
            gpr_rgb_buffer decoded={0};
            REQUIRE(vc5_decoder_process_wavelet_channel(&dp,&stock,channel,&decoded)==CODEC_ERROR_OKAY);
            REQUIRE(decoded.width==probe.width && decoded.height==probe.height);
            REQUIRE(!memcmp(decoded.buffer,probe.planes[channel],decoded.size));
            free(decoded.buffer);free(probe.planes[channel]);
        }
        free(stock.buffer);free(bytes);
    }
    free(raw);
    printf("PASS: %ux%u %u-bit format %d retained bands reconstruct exact quarter-scale stock planes at all six qualities\n",width,height,bits,(int)format);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--matrix")) {
        static const PIXEL_FORMAT formats[4][3]={
            {PIXEL_FORMAT_RAW_RGGB_12,PIXEL_FORMAT_RAW_RGGB_14,PIXEL_FORMAT_RAW_RGGB_16},
            {PIXEL_FORMAT_RAW_GBRG_12,PIXEL_FORMAT_RAW_GBRG_14,PIXEL_FORMAT_RAW_GBRG_16},
            {PIXEL_FORMAT_RAW_GRBG_12,PIXEL_FORMAT_RAW_GRBG_14,PIXEL_FORMAT_RAW_GRBG_16},
            {PIXEL_FORMAT_RAW_BGGR_12,PIXEL_FORMAT_RAW_BGGR_14,PIXEL_FORMAT_RAW_BGGR_16}};
        static const VC5_ENCODER_PIXEL_FORMAT public_formats[4][3]={
            {VC5_ENCODER_PIXEL_FORMAT_RGGB_12,VC5_ENCODER_PIXEL_FORMAT_RGGB_14,VC5_ENCODER_PIXEL_FORMAT_RGGB_16},
            {VC5_ENCODER_PIXEL_FORMAT_GBRG_12,VC5_ENCODER_PIXEL_FORMAT_GBRG_14,VC5_ENCODER_PIXEL_FORMAT_GBRG_16},
            {VC5_ENCODER_PIXEL_FORMAT_GRBG_12,VC5_ENCODER_PIXEL_FORMAT_GRBG_14,VC5_ENCODER_PIXEL_FORMAT_GRBG_16},
            {VC5_ENCODER_PIXEL_FORMAT_BGGR_12,VC5_ENCODER_PIXEL_FORMAT_BGGR_14,VC5_ENCODER_PIXEL_FORMAT_BGGR_16}};
        // Packed output rows have odd pixel counts (and are not all 64-byte
        // aligned). Compare against the old padded-wavelet path and independent
        // entropy decoding at both a small and a wider tail geometry.
        for (unsigned phase=0;phase<4;++phase) for (unsigned depth=0;depth<3;++depth) {
            run_case(50,50,12+2*depth,formats[phase][depth],public_formats[phase][depth],NULL);
            run_case(130,98,12+2*depth,formats[phase][depth],public_formats[phase][depth],NULL);
        }
    } else run_case(argc>1 ? 4096:128,argc>1 ? 3072:96,14,
        PIXEL_FORMAT_RAW_RGGB_14,VC5_ENCODER_PIXEL_FORMAT_RGGB_14,argc>1 ? argv[1]:NULL);
    return 0;
}
