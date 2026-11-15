/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/* Deliberately compiled as C: C++ overloads must not mask missing C symbols. */
#include "gpr.h"
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition) do { if (!(condition)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition);exit(1); } } while (0)
static void *live[8];
static unsigned calls,fail_at=~0u,allocated;
static void *allocate(size_t size) {
    void *pointer;
    if (calls++==fail_at) return NULL;
    REQUIRE(allocated<8);
    pointer=malloc(size);REQUIRE(pointer!=NULL);live[allocated++]=pointer;return pointer;
}
static void release(void *pointer) {
    unsigned i;
    if (!pointer) return;
    for (i=0;i<allocated;++i) if (live[i]==pointer) break;
    REQUIRE(i<allocated);live[i]=live[--allocated];free(pointer);
}
static void copy_test(void) {
    gpr_parameters source,target;
    gpr_allocator allocator={allocate,release};
    unsigned i,failure;
    unsigned char jpeg[4]={1,2,3,4};
    gpr_parameters_set_defaults(&source);
    source.input_width=1234;source.exif_info.iso_speed_rating=400;
    source.gpmf_payload.size=23;source.gpmf_payload.buffer=malloc(23);REQUIRE(source.gpmf_payload.buffer);
    memset(source.gpmf_payload.buffer,0x53,23);
    source.tuning_info.gain_map.size=16;
    for (i=0;i<4;++i) {
        source.tuning_info.gain_map.buffers[i]=(char *)malloc(16);REQUIRE(source.tuning_info.gain_map.buffers[i]);
        memset(source.tuning_info.gain_map.buffers[i],(int)(i+1),16);
    }
    source.preview_image.jpg_preview.buffer=jpeg;source.preview_image.jpg_preview.size=sizeof(jpeg);
    for (failure=0;failure<5;++failure) {
        fail_at=failure;calls=0;
        REQUIRE(!gpr_parameters_construct_copy_with_allocator(&allocator,&source,&target));
        REQUIRE(!allocated && !target.gpmf_payload.buffer && !target.tuning_info.gain_map.size);
        for (i=0;i<4;++i) REQUIRE(!target.tuning_info.gain_map.buffers[i]);
        gpr_parameters_destroy(&target,release);
        REQUIRE(((unsigned char *)source.gpmf_payload.buffer)[0]==0x53);
    }
    fail_at=~0u;calls=0;
    REQUIRE(gpr_parameters_construct_copy_with_allocator(&allocator,&source,&target));
    REQUIRE(allocated==5 && calls==5 && target.input_width==1234 && target.exif_info.iso_speed_rating==400);
    REQUIRE(target.gpmf_payload.buffer!=source.gpmf_payload.buffer && !memcmp(target.gpmf_payload.buffer,source.gpmf_payload.buffer,23));
    for (i=0;i<4;++i) REQUIRE(target.tuning_info.gain_map.buffers[i]!=source.tuning_info.gain_map.buffers[i] &&
        !memcmp(target.tuning_info.gain_map.buffers[i],source.tuning_info.gain_map.buffers[i],16));
    REQUIRE(target.preview_image.jpg_preview.buffer==jpeg);
    REQUIRE(!gpr_parameters_construct_copy_with_allocator(&allocator,&target,&target));
    REQUIRE(allocated==5 && target.input_width==1234);
    gpr_parameters_destroy(&target,release);gpr_parameters_destroy(&target,release);REQUIRE(!allocated);

    gpr_parameters_construct_copy(&source,&target);
    REQUIRE(target.gpmf_payload.buffer && target.gpmf_payload.buffer!=source.gpmf_payload.buffer);
    REQUIRE(!memcmp(target.gpmf_payload.buffer,source.gpmf_payload.buffer,23));
    gpr_parameters_destroy(&target,free);gpr_parameters_destroy(&target,free);
    REQUIRE(!gpr_parameters_construct_copy_with_allocator(NULL,&source,&target) && !target.gpmf_payload.buffer);
    REQUIRE(!gpr_parameters_construct_copy_with_allocator(&allocator,NULL,&target));
    REQUIRE(!gpr_parameters_construct_copy_with_allocator(&allocator,&source,NULL));
    { void *saved=source.gpmf_payload.buffer;source.gpmf_payload.buffer=NULL;
      REQUIRE(!gpr_parameters_construct_copy_with_allocator(&allocator,&source,&target));source.gpmf_payload.buffer=saved; }
    gpr_parameters_destroy(&source,free);REQUIRE(jpeg[0]==1);
}
static void classify_test(const char *path) {
    gpr_parameters parameters;
    gpr_allocator allocator={malloc,free};
    gpr_buffer payload={0},gpr={0},dng={0},raw={0},bad={0};
    unsigned i;size_t offset;unsigned char junk[64]={0};
    FILE *file=fopen(path,"rb");long length;
    REQUIRE(file && !fseek(file,0,SEEK_END));length=ftell(file);REQUIRE(length>0 && !fseek(file,0,SEEK_SET));
    payload.size=(size_t)length;payload.buffer=malloc(payload.size);REQUIRE(payload.buffer);
    REQUIRE(fread(payload.buffer,1,payload.size,file)==payload.size);fclose(file);
    gpr_parameters_set_defaults(&parameters);parameters.input_width=128;parameters.input_height=96;
    parameters.input_pitch=256;parameters.enable_preview=false;parameters.tuning_info.pixel_format=PIXEL_FORMAT_RGGB_14;
    parameters.tuning_info.dgain_saturation_level.level_red=16383;
    parameters.tuning_info.dgain_saturation_level.level_green_even=16383;
    parameters.tuning_info.dgain_saturation_level.level_green_odd=16383;
    parameters.tuning_info.dgain_saturation_level.level_blue=16383;
    REQUIRE(gpr_convert_vc5_to_gpr(&allocator,&parameters,&payload,&gpr));
    REQUIRE(gpr_check_vc5(&gpr,malloc,free));
    raw.size=128*96*2;raw.buffer=malloc(raw.size);REQUIRE(raw.buffer);
    for (i=0;i<128*96;++i) ((uint16_t *)raw.buffer)[i]=(uint16_t)(i%16384);
    REQUIRE(gpr_convert_raw_to_dng(&allocator,&parameters,&raw,&dng));
    REQUIRE(!gpr_check_vc5(&dng,malloc,free));
    REQUIRE(!gpr_check_vc5(NULL,malloc,free));
    REQUIRE(!gpr_check_vc5(&bad,malloc,free));
    REQUIRE(!gpr_check_vc5(&gpr,NULL,free) && !gpr_check_vc5(&gpr,malloc,NULL));
    bad.buffer=junk;
    for (i=0;i<=sizeof(junk);++i) {bad.size=i;REQUIRE(!gpr_check_vc5(&bad,malloc,free));}
    bad.buffer=gpr.buffer;
    for (i=0;i<64;++i) {bad.size=i;REQUIRE(!gpr_check_vc5(&bad,malloc,free));}
    /* Classification explicitly does not validate/decode the image payload. */
    REQUIRE(gpr.size>=payload.size);
    for (offset=0;offset<=gpr.size-payload.size;++offset)
        if (!memcmp((char *)gpr.buffer+offset,payload.buffer,payload.size)) break;
    REQUIRE(offset<=gpr.size-payload.size);memset((char *)gpr.buffer+offset,0,payload.size);
    REQUIRE(gpr_check_vc5(&gpr,malloc,free));
    free(payload.buffer);free(gpr.buffer);free(dng.buffer);free(raw.buffer);gpr_parameters_destroy(&parameters,free);
}
int main(int argc,char **argv) {
    REQUIRE(argc==2);copy_test();classify_test(argv[1]);
    puts("PASS: public C symbols, owned parameter copies, all allocation failures, borrowed JPEG and metadata-only VC-5 classification");
    return 0;
}
