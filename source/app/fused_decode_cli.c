/* Standalone .gpr (FUSED bitstream) -> raw bayer decoder.
   For the Pi-capture -> desktop-CNN ship-readiness demo.
   Usage: fused_decode_cli in.gpr SENSOR_W SENSOR_H out.raw
          [4k_raw_1x|2k_raw_0p5x|2k_raw_0p5x_fast|2k_raw_0p5x_l2hh|mission1_preview_4x_1024x768|mission1_preview_rgb_1024x768]
*/
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

extern int gpr_decode_fused(const uint8_t *enc, size_t enc_size,
                            uint16_t *bayer_out, size_t bayer_pitch_bytes,
                            int *out_width, int *out_height);
extern int gpr_decode_fused_halfres(const uint8_t *enc, size_t enc_size,
                                    uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                    int *out_width, int *out_height);
extern int gpr_decode_fused_ll_preview(const uint8_t *enc, size_t enc_size,
                                       uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                       int *out_width, int *out_height);
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static uint16_t avg4_u16(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    return (uint16_t)(((uint32_t)a + (uint32_t)b + (uint32_t)c + (uint32_t)d + 2u) >> 2);
}

static int percentile_u16_hist(const uint32_t hist[16384], uint32_t total, double frac) {
    uint32_t want = (uint32_t)((double)(total - 1) * frac + 0.5);
    uint32_t seen = 0;
    for (int i = 0; i < 16384; i++) {
        seen += hist[i];
        if (seen > want) return i;
    }
    return 16383;
}

static uint8_t clamp_u8_float(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

static int mission1_preview_isp_rgb24(const uint16_t *raw, int width, int height,
                                      int pitch_pixels, uint8_t *rgb_out) {
    if (!raw || !rgb_out || width <= 0 || height <= 0 || (width & 1) || (height & 1)) return -1;

    double r_sum = 0.0, g_sum = 0.0, b_sum = 0.0;
    uint32_t blocks = 0;
    for (int y = 0; y < height; y += 2) {
        const uint16_t *row0 = raw + (size_t)y * pitch_pixels;
        const uint16_t *row1 = row0 + pitch_pixels;
        for (int x = 0; x < width; x += 2) {
            r_sum += row0[x];
            g_sum += ((double)row0[x + 1] + (double)row1[x]) * 0.5;
            b_sum += row1[x + 1];
            blocks++;
        }
    }
    float r_gain = (float)((g_sum / blocks) / fmax(r_sum / blocks, 1.0));
    float b_gain = (float)((g_sum / blocks) / fmax(b_sum / blocks, 1.0));
    if (r_gain < 0.25f) r_gain = 0.25f;
    if (r_gain > 4.0f) r_gain = 4.0f;
    if (b_gain < 0.25f) b_gain = 0.25f;
    if (b_gain > 4.0f) b_gain = 4.0f;

    uint32_t hist[16384];
    memset(hist, 0, sizeof(hist));
    for (int y = 0; y < height; y += 2) {
        const uint16_t *row0 = raw + (size_t)y * pitch_pixels;
        const uint16_t *row1 = row0 + pitch_pixels;
        for (int x = 0; x < width; x += 2) {
            float r = row0[x] * r_gain;
            float g = ((float)row0[x + 1] + (float)row1[x]) * 0.5f;
            float b = row1[x + 1] * b_gain;
            int lum = (int)(0.2126f * r + 0.7152f * g + 0.0722f * b + 0.5f);
            if (lum < 0) lum = 0;
            if (lum > 16383) lum = 16383;
            hist[lum]++;
        }
    }

    int lo = percentile_u16_hist(hist, blocks, 0.008);
    int hi = percentile_u16_hist(hist, blocks, 0.998);
    if (hi <= lo + 64) {
        lo = 0;
        hi = 16383;
    }
    uint8_t lut[16384];
    float inv = 1.0f / (float)(hi - lo);
    for (int i = 0; i < 16384; i++) {
        float x = ((float)i - (float)lo) * inv;
        if (x < 0.0f) x = 0.0f;
        if (x > 1.0f) x = 1.0f;
        x = powf(x, 1.0f / 2.25f);
        const float c = 1.18f;
        float xp = powf(x, c);
        float yp = powf(1.0f - x, c);
        x = xp / (xp + yp + 1.0e-9f);
        lut[i] = clamp_u8_float(x * 255.0f);
    }

    for (int y = 0; y < height; y += 2) {
        const uint16_t *row0 = raw + (size_t)y * pitch_pixels;
        const uint16_t *row1 = row0 + pitch_pixels;
        uint8_t *out0 = rgb_out + (size_t)y * width * 3;
        uint8_t *out1 = out0 + (size_t)width * 3;
        for (int x = 0; x < width; x += 2) {
            int rv = (int)(row0[x] * r_gain + 0.5f);
            int gv = (int)(((float)row0[x + 1] + (float)row1[x]) * 0.5f + 0.5f);
            int bv = (int)(row1[x + 1] * b_gain + 0.5f);
            if (rv < 0) rv = 0; if (rv > 16383) rv = 16383;
            if (gv < 0) gv = 0; if (gv > 16383) gv = 16383;
            if (bv < 0) bv = 0; if (bv > 16383) bv = 16383;
            float r = lut[rv], g = lut[gv], b = lut[bv];
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = lum + (r - lum) * 1.12f;
            g = lum + (g - lum) * 1.12f;
            b = lum + (b - lum) * 1.12f;
            uint8_t r8 = clamp_u8_float(r);
            uint8_t g8 = clamp_u8_float(g);
            uint8_t b8 = clamp_u8_float(b);
            size_t p = (size_t)x * 3;
            out0[p + 0] = r8; out0[p + 1] = g8; out0[p + 2] = b8;
            out0[p + 3] = r8; out0[p + 4] = g8; out0[p + 5] = b8;
            out1[p + 0] = r8; out1[p + 1] = g8; out1[p + 2] = b8;
            out1[p + 3] = r8; out1[p + 4] = g8; out1[p + 5] = b8;
        }
    }
    return 0;
}

static int downsample_bayer_0p5x_cfa(const uint16_t *src, int src_w, int src_h,
                                     int src_pitch_pixels, uint16_t *dst,
                                     int *dst_w, int *dst_h) {
    if (!src || !dst || !dst_w || !dst_h) return -1;
    if ((src_w % 4) != 0 || (src_h % 4) != 0) return -2;

    int out_w = src_w / 2;
    int out_h = src_h / 2;
    for (int y = 0; y < out_h; y++) {
        int phase_y = y & 1;
        int plane_y = y >> 1;
        int sy0 = phase_y + plane_y * 4;
        int sy1 = sy0 + 2;
        const uint16_t *row0 = src + (size_t)sy0 * src_pitch_pixels;
        const uint16_t *row1 = src + (size_t)sy1 * src_pitch_pixels;
        uint16_t *out_row = dst + (size_t)y * out_w;
        for (int x = 0; x < out_w; x++) {
            int phase_x = x & 1;
            int plane_x = x >> 1;
            int sx0 = phase_x + plane_x * 4;
            int sx1 = sx0 + 2;
            out_row[x] = avg4_u16(row0[sx0], row0[sx1], row1[sx0], row1[sx1]);
        }
    }
    *dst_w = out_w;
    *dst_h = out_h;
    return 0;
}

static int downsample_bayer_0p25x_cfa(const uint16_t *src, int src_w, int src_h,
                                      int src_pitch_pixels, uint16_t *dst,
                                      int *dst_w, int *dst_h) {
    if (!src || !dst || !dst_w || !dst_h) return -1;
    if ((src_w % 8) != 0 || (src_h % 8) != 0) return -2;

    int out_w = src_w / 4;
    int out_h = src_h / 4;
    for (int y = 0; y < out_h; y++) {
        int phase_y = y & 1;
        int plane_y = y >> 1;
        uint16_t *out_row = dst + (size_t)y * out_w;
        for (int x = 0; x < out_w; x++) {
            int phase_x = x & 1;
            int plane_x = x >> 1;
            int sy0 = phase_y + plane_y * 8;
            int sx0 = phase_x + plane_x * 8;
            uint32_t sum = 0;
            for (int ky = 0; ky < 4; ky++) {
                const uint16_t *row = src + (size_t)(sy0 + ky * 2) * src_pitch_pixels;
                for (int kx = 0; kx < 4; kx++) {
                    sum += row[sx0 + kx * 2];
                }
            }
            out_row[x] = (uint16_t)((sum + 8u) >> 4);
        }
    }
    *dst_w = out_w;
    *dst_h = out_h;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s in.gpr SENSOR_W SENSOR_H out.raw "
                "[4k_raw_1x|2k_raw_0p5x|2k_raw_0p5x_fast|2k_raw_0p5x_l2hh|mission1_preview_4x_1024x768|mission1_preview_rgb_1024x768]\n", argv[0]);
        return 1;
    }
    const char *target = (argc >= 6) ? argv[5] : "4k_raw_1x";
    int target_2k_env = strcmp(target, "2k_raw_0p5x") == 0;
    int target_2k_fast = strcmp(target, "2k_raw_0p5x_fast") == 0;
    int target_2k_l2hh = strcmp(target, "2k_raw_0p5x_l2hh") == 0;
    int target_screen_preview =
        strcmp(target, "mission1_preview_4x_1024x768") == 0 ||
        strcmp(target, "mission1_screen_preview_960x720") == 0 ||
        strcmp(target, "mission1_preview_rgb_1024x768") == 0;
    int target_preview_rgb = strcmp(target, "mission1_preview_rgb_1024x768") == 0;
    int target_2k = target_2k_env || target_2k_fast || target_2k_l2hh;
    if (!target_2k && !target_screen_preview && strcmp(target, "4k_raw_1x") != 0) {
        fprintf(stderr, "unknown target '%s' (expected 4k_raw_1x, 2k_raw_0p5x, "
                "2k_raw_0p5x_fast, 2k_raw_0p5x_l2hh, mission1_preview_4x_1024x768, "
                "or mission1_preview_rgb_1024x768)\n", target);
        return 1;
    }
    if (target_2k_fast) {
        setenv("GPR_DECODE_HALFRES_DROP_L2_HP", "1", 1);
        unsetenv("GPR_DECODE_HALFRES_L2_MASK");
        setenv("GPR_DECODE_HALFRES_STREAM", "1", 1);
        setenv("GPR_DECODE_FUSED_STREAM_STRIPS", "2", 1);
    } else if (target_2k_l2hh) {
        unsetenv("GPR_DECODE_HALFRES_DROP_L2_HP");
        setenv("GPR_DECODE_HALFRES_L2_MASK", "4", 1);
        setenv("GPR_DECODE_HALFRES_STREAM", "1", 1);
        setenv("GPR_DECODE_FUSED_STREAM_STRIPS", "2", 1);
    }
    int sw = atoi(argv[2]), sh = atoi(argv[3]);
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("open in"); return 1; }
    fseek(f, 0, SEEK_END); size_t sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *enc = malloc(sz);
    if (fread(enc, 1, sz, f) != sz) { fprintf(stderr, "read fail\n"); free(enc); fclose(f); return 1; }
    fclose(f);
    /* Allocate at sensor dims; decoder may emit half-res for decimated capture
       — still fits in this buffer. Bayer pitch = full sensor width. */
    uint16_t *bayer = (uint16_t *)calloc((size_t)sw * sh, sizeof(uint16_t));
    if (!bayer) { fprintf(stderr, "alloc fail\n"); return 1; }
    int ow = 0, oh = 0;
    double t0 = now_ms();
    if (target_screen_preview) {
        /* Full-frame preview: decode the preserved LL bands directly to a
           1024x768 Bayer preview. Any 960x720 screen fit belongs in the
           display layer. */
    }
    int rc = target_screen_preview
        ? gpr_decode_fused_ll_preview(enc, sz, bayer, (size_t)sw * sizeof(uint16_t), &ow, &oh)
        : (target_2k
            ? gpr_decode_fused_halfres(enc, sz, bayer, (size_t)sw * sizeof(uint16_t), &ow, &oh)
            : gpr_decode_fused(enc, sz, bayer, (size_t)sw * sizeof(uint16_t), &ow, &oh));
    double t = now_ms() - t0;
    if (rc != 0) { fprintf(stderr, "decode failed rc=%d\n", rc); free(bayer); free(enc); return 2; }
    fprintf(stderr, "DECODE: %dx%d in %.1f ms (sensor %dx%d, in %zu bytes)\n",
            ow, oh, t, sw, sh, sz);

    uint16_t *out_bayer = bayer;
    int out_w = ow, out_h = oh, out_pitch = sw;
    double target_ms = 0.0;
    uint8_t *out_rgb = NULL;
    int output_rgb = 0;
    if (target_screen_preview && target_preview_rgb && (ow == sw / 4) && (oh == sh / 4)) {
        out_rgb = (uint8_t *)malloc((size_t)ow * oh * 3);
        if (!out_rgb) { fprintf(stderr, "target rgb alloc fail\n"); free(bayer); free(enc); return 1; }
        double td0 = now_ms();
        int trc = mission1_preview_isp_rgb24(bayer, ow, oh, sw, out_rgb);
        target_ms = now_ms() - td0;
        if (trc != 0) {
            fprintf(stderr, "target preview isp failed rc=%d\n", trc);
            free(out_rgb); free(bayer); free(enc);
            return 3;
        }
        output_rgb = 1;
        out_w = ow;
        out_h = oh;
        fprintf(stderr, "TARGET: %s %dx%d in %.1f ms\n", target, out_w, out_h, target_ms);
    } else if (target_screen_preview && (ow == sw / 4) && (oh == sh / 4)) {
        fprintf(stderr, "TARGET: %s %dx%d in %.1f ms\n", target, out_w, out_h, target_ms);
    } else if (target_screen_preview) {
        out_w = ow / 4;
        out_h = oh / 4;
        out_pitch = out_w;
        out_bayer = (uint16_t *)malloc((size_t)out_w * out_h * sizeof(uint16_t));
        if (!out_bayer) { fprintf(stderr, "target alloc fail\n"); free(bayer); free(enc); return 1; }
        double td0 = now_ms();
        int trc = downsample_bayer_0p25x_cfa(bayer, ow, oh, sw, out_bayer, &out_w, &out_h);
        target_ms = now_ms() - td0;
        if (trc != 0) {
            fprintf(stderr, "target screen preview failed rc=%d\n", trc);
            free(out_bayer); free(bayer); free(enc);
            return 3;
        }
        fprintf(stderr, "TARGET: %s %dx%d in %.1f ms\n", target, out_w, out_h, target_ms);
    } else if (target_2k && (ow != sw / 4 || oh != sh / 4)) {
        out_w = ow / 2;
        out_h = oh / 2;
        out_pitch = out_w;
        out_bayer = (uint16_t *)malloc((size_t)out_w * out_h * sizeof(uint16_t));
        if (!out_bayer) { fprintf(stderr, "target alloc fail\n"); free(bayer); free(enc); return 1; }
        double td0 = now_ms();
        int trc = downsample_bayer_0p5x_cfa(bayer, ow, oh, sw, out_bayer, &out_w, &out_h);
        target_ms = now_ms() - td0;
        if (trc != 0) {
            fprintf(stderr, "target downsample failed rc=%d\n", trc);
            free(out_bayer); free(bayer); free(enc);
            return 3;
        }
        fprintf(stderr, "TARGET: %s %dx%d in %.1f ms\n", target, out_w, out_h, target_ms);
    } else {
        fprintf(stderr, "TARGET: %s %dx%d in %.1f ms\n", target, out_w, out_h, target_ms);
    }

    /* Write only the target rect (handle pitch != width for direct decode). */
    FILE *fo = fopen(argv[4], "wb");
    if (!fo) {
        perror("open out");
        if (out_rgb) free(out_rgb);
        if (out_bayer != bayer) free(out_bayer);
        free(bayer); free(enc);
        return 1;
    }
    if (output_rgb) {
        fwrite(out_rgb, 1, (size_t)out_w * out_h * 3, fo);
    } else {
        for (int y = 0; y < out_h; y++) {
            fwrite(out_bayer + (size_t)y * out_pitch, sizeof(uint16_t), out_w, fo);
        }
    }
    fclose(fo);
    if (out_rgb) free(out_rgb);
    if (out_bayer != bayer) free(out_bayer);
    free(bayer); free(enc);
    return 0;
}
