/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_BAYER_H
#define GPR_BAYER_H

#include "gpr_tuning_info.h"
#include "dng_ifd.h"
#if GPR_READING
#include "vc5_decoder.h"
#endif

struct gpr_bayer_layout
{
    unsigned phase; // DNG SetBayerMosaic: GRBG=0, RGGB=1, BGGR=2, GBRG=3.
    unsigned bits;
    bool packed;
    unsigned red, green_red, green_blue, blue; // Physical positions in a 2x2 tile.
};

static inline bool gpr_get_bayer_layout(GPR_PIXEL_FORMAT format, gpr_bayer_layout &out)
{
    // Green labels here follow the row's red/blue site, independently of the
    // codec's G1/G2 transform convention.
    static const gpr_bayer_layout table[] = {
        {1,12,false,0,1,2,3}, {1,12,true,0,1,2,3}, {1,14,false,0,1,2,3},
        {3,12,false,2,3,0,1}, {3,12,true,2,3,0,1},
        {1,16,false,0,1,2,3}, {3,16,false,2,3,0,1}, {3,14,false,2,3,0,1},
        {0,12,false,1,0,3,2}, {0,14,false,1,0,3,2}, {0,16,false,1,0,3,2},
        {2,12,false,3,2,1,0}, {2,14,false,3,2,1,0}, {2,16,false,3,2,1,0}
    };
    unsigned index = static_cast<unsigned>(format);
    if (index >= sizeof(table)/sizeof(table[0])) return false;
    out = table[index];
    return true;
}

static inline bool gpr_bayer_from_ifd(const dng_ifd &ifd, GPR_PIXEL_FORMAT &format)
{
    if (ifd.fSamplesPerPixel != 1 || ifd.fSampleFormat[0] != 1 ||
        ifd.fBitsPerSample[0] > 16 || ifd.fCFARepeatPatternRows != 2 ||
        ifd.fCFARepeatPatternCols != 2 || ifd.fCFALayout != 1) return false;
    // Public matrices and white-balance fields use canonical RGB plane order.
    if (ifd.fCFAPlaneColor[0]!=0 || ifd.fCFAPlaneColor[1]!=1 || ifd.fCFAPlaneColor[2]!=2) return false;
    unsigned pattern = 0;
    for (unsigned y=0;y<2;++y) for (unsigned x=0;x<2;++x) {
        unsigned plane = ifd.fCFAPattern[y][x];
        if (plane >= kMaxColorPlanes) return false;
        unsigned color = ifd.fCFAPlaneColor[plane];
        if (color > 2) return false;
        pattern = (pattern << 2) | color;
    }
    // RGB color codes are 0,1,2; do not treat an unknown pattern as GBRG.
    unsigned phase;
    switch (pattern) {
        case 0x16: phase=1; break; // 0,1,1,2
        case 0x61: phase=3; break; // 1,2,0,1
        case 0x49: phase=0; break; // 1,0,2,1
        case 0x94: phase=2; break; // 2,1,1,0
        default: return false;
    }
    // Historical GPR stores 16-bit words even for 12/14-bit counts. WhiteLevel
    // identifies those count domains, not BitsPerSample. Do not guess when a
    // nonstandard WhiteLevel makes the nominal domain ambiguous.
    const double white = ifd.fWhiteLevel[0];
    unsigned column;
    if (white == 4095) column=0;
    else if (white == 16383) column=1;
    else if (white == 65535) column=2;
    else return false;
    if (ifd.fBitsPerSample[0] < (column==0?12u:column==1?14u:16u)) return false;
    static const GPR_PIXEL_FORMAT formats[4][3] = {
        {PIXEL_FORMAT_GRBG_12,PIXEL_FORMAT_GRBG_14,PIXEL_FORMAT_GRBG_16},
        {PIXEL_FORMAT_RGGB_12,PIXEL_FORMAT_RGGB_14,PIXEL_FORMAT_RGGB_16},
        {PIXEL_FORMAT_BGGR_12,PIXEL_FORMAT_BGGR_14,PIXEL_FORMAT_BGGR_16},
        {PIXEL_FORMAT_GBRG_12,PIXEL_FORMAT_GBRG_14,PIXEL_FORMAT_GBRG_16}
    };
    format=formats[phase][column];
    return true;
}

#if GPR_READING
static inline bool gpr_get_decoder_format(GPR_PIXEL_FORMAT format, VC5_DECODER_PIXEL_FORMAT &out)
{
    static const VC5_DECODER_PIXEL_FORMAT table[] = {
        VC5_DECODER_PIXEL_FORMAT_RGGB_12, VC5_DECODER_PIXEL_FORMAT_RGGB_12,
        VC5_DECODER_PIXEL_FORMAT_RGGB_14, VC5_DECODER_PIXEL_FORMAT_GBRG_12,
        VC5_DECODER_PIXEL_FORMAT_GBRG_12, VC5_DECODER_PIXEL_FORMAT_RGGB_16,
        VC5_DECODER_PIXEL_FORMAT_GBRG_16, VC5_DECODER_PIXEL_FORMAT_GBRG_14,
        VC5_DECODER_PIXEL_FORMAT_GRBG_12, VC5_DECODER_PIXEL_FORMAT_GRBG_14,
        VC5_DECODER_PIXEL_FORMAT_GRBG_16, VC5_DECODER_PIXEL_FORMAT_BGGR_12,
        VC5_DECODER_PIXEL_FORMAT_BGGR_14, VC5_DECODER_PIXEL_FORMAT_BGGR_16
    };
    unsigned index=static_cast<unsigned>(format);
    if (index>=sizeof(table)/sizeof(table[0])) return false;
    out=table[index];
    return true;
}
#endif
#endif
