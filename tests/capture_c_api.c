/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_capture.h"
#include <string.h>
static unsigned writes;
static int unexpected_write(void *user,const uint8_t *bytes,size_t size) {
    (void)user;(void)bytes;(void)size;++writes;return 0;
}

static uint32_t slow_crc(uint32_t crc,const uint8_t *p,size_t n) {
    crc ^= UINT32_MAX;
    while (n--) {
        crc ^= *p++;
        for (unsigned bit=0;bit<8;++bit) crc=(crc>>1) ^ ((crc&1) ? 0xedb88320u : 0);
    }
    return crc ^ UINT32_MAX;
}

int main(void) {
    gpr_capture *capture=(gpr_capture *)(uintptr_t)1;
    if (gpr_capture_create(NULL,NULL,&capture)!=GPR_CAPTURE_INVALID || capture!=NULL) return 1;
    if (gpr_capture_create_with_observer(NULL,NULL,NULL,NULL,&capture)!=GPR_CAPTURE_INVALID || capture!=NULL) return 9;
    if (gpr_capture_create_with_encoder_preview(NULL,NULL,NULL,NULL,NULL,NULL,&capture)!=GPR_CAPTURE_INVALID || capture!=NULL) return 10;
    if (gpr_capture_create_with_encoder_rows(NULL,NULL,NULL,NULL,NULL,NULL,&capture)!=GPR_CAPTURE_INVALID || capture!=NULL) return 11;
    if (gpr_capture_flush(NULL)!=GPR_CAPTURE_INVALID || gpr_capture_finish(NULL)!=GPR_CAPTURE_INVALID) return 2;
    if (gpr_capture_get_stats(NULL,NULL)!=GPR_CAPTURE_INVALID) return 3;
    if (gpr_gvid_crc32(0,"123456789",9)!=0xcbf43926u) return 4;
    {
        gpr_capture_config config={0};gpr_capture_callbacks callbacks={unexpected_write,0,0};
        config.video=(gpr_gvid_config){128,96,30,1,3,14,{'R','G','G','B'}};
        config.stride_bytes=256;config.max_inflight=1;
        for (unsigned dimension=8;dimension<48;dimension+=8) {
            config.video.width=dimension;
            if (gpr_capture_create(&config,&callbacks,&capture)!=GPR_CAPTURE_INVALID || capture) return 6;
            config.video.width=128;config.video.height=dimension;
            if (gpr_capture_create(&config,&callbacks,&capture)!=GPR_CAPTURE_INVALID || capture) return 7;
            config.video.height=96;
        }
        if (writes) return 8;
    }
    uint8_t data[1040];
    for (size_t i=0;i<sizeof data;++i) data[i]=(uint8_t)(i*13+7);
    for (unsigned offset=0;offset<8;++offset) for (size_t n=0;n<=1024;++n) {
        uint32_t seed=(uint32_t)(n*171871u);
        if (gpr_gvid_crc32(seed,data+offset,n)!=slow_crc(seed,data+offset,n)) return 5;
    }
    gpr_capture_cancel(NULL); gpr_capture_destroy(NULL);
    return 0;
}
