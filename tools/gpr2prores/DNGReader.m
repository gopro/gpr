// DNGReader.m — LibRaw-based Bayer extractor.
//
// We dynamically link to LibRaw for decoding all
// the camera-specific raw formats (Z8 DNGs use lossless JPEG = compression
// type 7, which a hand-rolled TIFF reader can't handle).
//
// We open the file, call libraw_unpack(), and copy the raw_image[] uint16
// buffer into a fresh allocation. Black/white levels and CFA filter come
// from the LibRaw color/sizes structs.

#import "DNGReader.h"
#import <libraw/libraw.h>

@implementation DNGReader

+ (uint16_t *)readBayerFromPath:(NSString *)path info:(DNGInfo *)info {
    libraw_data_t *lr = libraw_init(0);
    if (!lr) { fprintf(stderr, "libraw_init failed\n"); return NULL; }

    int rc = libraw_open_file(lr, [path UTF8String]);
    if (rc != 0) {
        fprintf(stderr, "libraw_open_file(%s) rc=%d\n", [path UTF8String], rc);
        libraw_close(lr);
        return NULL;
    }
    rc = libraw_unpack(lr);
    if (rc != 0) {
        fprintf(stderr, "libraw_unpack rc=%d\n", rc);
        libraw_close(lr);
        return NULL;
    }
    if (!lr->rawdata.raw_image) {
        fprintf(stderr, "no raw_image\n");
        libraw_close(lr);
        return NULL;
    }

    uint32_t rw = lr->sizes.raw_width;
    uint32_t rh = lr->sizes.raw_height;
    // Some sensors have margins; the active area is at (top_margin, left_margin)
    // with size width x height. For the Z8 the raw_image is the full padded
    // sensor including masked pixels (~5520 x 8280 ≈ 5552 x 8312 actual).
    // The Python pipeline uses raw_image directly via rawpy.imread().raw_image,
    // which is the full raw_width x raw_height with no crop. Match that.

    size_t pixels = (size_t)rw * rh;
    uint16_t *buf = malloc(pixels * sizeof(uint16_t));
    if (!buf) { libraw_close(lr); return NULL; }
    memcpy(buf, lr->rawdata.raw_image, pixels * sizeof(uint16_t));

    info->width = rw;
    info->height = rh;
    info->bitsPerSample = 16; // libraw delivers 16-bit container regardless
    // Filter pattern: LibRaw codes per-position color as R=0, G=1, B=2, G2=3.
    // We map to our DNG-style enum: 0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR.
    unsigned filters = lr->idata.filters;
    unsigned c00 = (filters >> 0) & 3;
    unsigned c01 = (filters >> 2) & 3;
    unsigned c10 = (filters >> 4) & 3;
    // Treat G2 (3) as G (1) for pattern detection.
    unsigned C00 = (c00 == 3) ? 1 : c00;
    unsigned C01 = (c01 == 3) ? 1 : c01;
    unsigned C10 = (c10 == 3) ? 1 : c10;
    if      (C00 == 0 && C01 == 1 && C10 == 1) info->cfaPattern = 0; // RGGB
    else if (C00 == 1 && C01 == 2 && C10 == 0) info->cfaPattern = 1; // GBRG
    else if (C00 == 1 && C01 == 0 && C10 == 2) info->cfaPattern = 2; // GRBG
    else if (C00 == 2 && C01 == 1 && C10 == 1) info->cfaPattern = 3; // BGGR
    else info->cfaPattern = 0;

    // Black level: per LibRaw, the total per-channel black is:
    //   color.black + color.cblack[ch_index] + color.cblack[6 + pos]
    // where pos walks the cblack[4]*cblack[5] pattern. For the Z8:
    //   color.black=0, cblack[0..3]=0,0,0,0, cblack[4..5]=2,2,
    //   cblack[6..9]=1008,1008,1008,1008.
    // We use a single scalar (mean across pattern positions), which is fine
    // when all four sub-blacks are equal (Z8 case).
    unsigned pat_w = lr->color.cblack[4];
    unsigned pat_h = lr->color.cblack[5];
    unsigned bl_sum = 0;
    int bl_n = 0;
    if (pat_w && pat_h && (pat_w * pat_h) <= 32) {
        for (unsigned i = 0; i < pat_w * pat_h; i++) {
            unsigned ch = i % 4;
            bl_sum += lr->color.black + lr->color.cblack[ch] + lr->color.cblack[6 + i];
            bl_n++;
        }
    } else {
        bl_sum = lr->color.black;
        bl_n = 1;
    }
    info->blackLevel = bl_n ? (bl_sum / bl_n) : 0;
    info->whiteLevel = lr->color.maximum ? lr->color.maximum : 16383;
    float gMul = lr->color.cam_mul[1] > 0 ? lr->color.cam_mul[1] : 1.0f;
    info->wbR = lr->color.cam_mul[0] / gMul;
    info->wbG = 1.0f;
    info->wbB = lr->color.cam_mul[2] / gMul;
    if (info->wbR <= 0) info->wbR = 1.0f;
    if (info->wbB <= 0) info->wbB = 1.0f;

    // Color matrix: camera-RGB → sRGB. LibRaw stores rgb_cam as 3x4, with the
    // 4th column being the second-green channel weighting (we average G & G2
    // upstream so we treat the matrix as 3x3 with the G column).
    for (int r = 0; r < 3; r++) {
        info->rgb_cam[r][0] = lr->color.rgb_cam[r][0];
        info->rgb_cam[r][1] = lr->color.rgb_cam[r][1];
        info->rgb_cam[r][2] = lr->color.rgb_cam[r][2];
    }
    // If rgb_cam is all zeros (some unprocessed DNGs), fall back to identity.
    float sum = 0;
    for (int r=0;r<3;r++) for(int c=0;c<3;c++) sum += fabsf(info->rgb_cam[r][c]);
    if (sum < 0.01f) {
        info->rgb_cam[0][0]=1; info->rgb_cam[0][1]=0; info->rgb_cam[0][2]=0;
        info->rgb_cam[1][0]=0; info->rgb_cam[1][1]=1; info->rgb_cam[1][2]=0;
        info->rgb_cam[2][0]=0; info->rgb_cam[2][1]=0; info->rgb_cam[2][2]=1;
    }

    libraw_close(lr);
    return buf;
}

@end
