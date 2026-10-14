/* test_multi_frame — encode → decode N raw frames through the fused codec,
 * write the decoded Bayer + a quick-and-dirty demosaiced PPM per frame, and
 * print per-frame encode/decode timings.
 *
 * The PPM is for sanity (looks roughly like the scene), NOT for visualization
 * quality. For correct color/gamma rendering use tools/render_compare.py on
 * the saved .raw files, which feeds them through rawpy/libraw with the source
 * DNG's white balance + color matrix + gamma. Rolling a demosaic by hand
 * always looks broken — don't trust the .ppm output for quality judgments.
 *
 * Honors the same env flags as bench_fused:
 *   GPR_INCLUDE_LL=1         single-level + LL band (decodable)
 *   GPR_ROW_DECIMATE=2       2x row decimation (channel-space)
 *   GPR_COL_DECIMATE=2       2x col decimation (channel-space)
 *   GPR_DROP_HIGHPASS=1      LL-only output (skips highpass tokenize)
 *
 * Build (Pi or any aarch64 with the encoder/decoder built):
 *   gcc -O2 source/app/test_multi_frame.c \
 *       build/source/lib/vc5_decoder/libvc5_decoder.a \
 *       build/source/lib/vc5_encoder/libvc5_encoder.a \
 *       build/source/lib/vc5_common/libvc5_common.a \
 *       build/source/lib/common/libcommon.a \
 *       -lpthread -lm -o /tmp/multi_frame
 *
 * Usage:
 *   multi_frame W H out_prefix raw1 [raw2 ...]
 *   GPR_INCLUDE_LL=1 GPR_ROW_DECIMATE=2 GPR_COL_DECIMATE=2 \
 *       multi_frame 8280 5520 /tmp/out/f frame1.raw frame2.raw ...
 *
 * Driven end-to-end by tools/run_codec_movie.sh which handles DNG→raw on
 * the local side, ssh's the raws over, runs this tool, and renders the
 * output through rawpy back on the local side.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef struct FUSED_ENCODER FUSED_ENCODER;
extern FUSED_ENCODER *gpr_encode_fused_create(int w, int h, int pf, int q);
extern int gpr_encode_fused_frame(FUSED_ENCODER *ctx, const unsigned char *raw, size_t sz,
                                   unsigned char **out, size_t *out_sz);
extern void gpr_encode_fused_destroy(FUSED_ENCODER *ctx);
extern int gpr_decode_fused(const unsigned char *enc, size_t enc_size,
                             uint16_t *bayer_out, size_t bayer_pitch_bytes,
                             int *out_width, int *out_height);

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int demosaic_to_ppm(const uint16_t *bayer, int W, int H,
                            int gain, const char *path)
{
    int ow = W / 2, oh = H / 2;
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    if (fprintf(f, "P6\n%d %d\n65535\n", ow, oh) < 0) {
        fclose(f); return 1;
    }
    uint8_t *row = malloc((size_t)ow * 6);
    if (!row) { fclose(f); return 1; }
    for (int yo = 0; yo < oh; yo++) {
        const uint16_t *r1 = bayer + (size_t)(yo * 2) * W;
        const uint16_t *r2 = bayer + (size_t)(yo * 2 + 1) * W;
        for (int xo = 0; xo < ow; xo++) {
            int xi = xo * 2;
            uint32_t r  = r1[xi];
            uint32_t g1 = r1[xi + 1];
            uint32_t g2 = r2[xi];
            uint32_t b  = r2[xi + 1];
            uint32_t g  = (g1 + g2) >> 1;
            uint32_t rr = r * gain; if (rr > 65535) rr = 65535;
            uint32_t gg = g * gain; if (gg > 65535) gg = 65535;
            uint32_t bb = b * gain; if (bb > 65535) bb = 65535;
            row[xo * 6 + 0] = (rr >> 8) & 0xff; row[xo * 6 + 1] = rr & 0xff;
            row[xo * 6 + 2] = (gg >> 8) & 0xff; row[xo * 6 + 3] = gg & 0xff;
            row[xo * 6 + 4] = (bb >> 8) & 0xff; row[xo * 6 + 5] = bb & 0xff;
        }
        if (fwrite(row, 1, (size_t)ow * 6, f) != (size_t)ow * 6) {
            free(row); fclose(f); return 1;
        }
    }
    free(row);
    return fclose(f) != 0;
}

static int write_buffer(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    int failed = fwrite(data, 1, size, f) != size;
    if (fclose(f) != 0) failed = 1;
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s W H out_prefix raw1 [raw2 ...]\n", argv[0]);
        return 1;
    }
    int W = atoi(argv[1]), H = atoi(argv[2]);
    if (W <= 0 || H <= 0 || (size_t)W > SIZE_MAX / 2 / (size_t)H) {
        fprintf(stderr, "invalid dimensions\n"); return 1;
    }
    const char *prefix = argv[3];
    int nframes = argc - 4;
    size_t in_sz = (size_t)W * H * 2;

    FUSED_ENCODER *enc = gpr_encode_fused_create(W, H, 1, 3);
    if (!enc) { fprintf(stderr, "encoder create fail\n"); return 1; }

    uint16_t *raw = malloc(in_sz);
    uint16_t *dec = calloc(1, in_sz);  /* worst case full res */
    int failed = 0;
    if (!raw || !dec) { failed = 1; goto cleanup; }

    /* B4/B5 (mlockall / MADV_HUGEPAGE) measured to regress on Pi 5;
       mlockall+MCL_CURRENT alone gave +5 ms median + higher variance,
       MADV_HUGEPAGE gave +60 ms (THP first-touch zeroing). Not adopted. */

    /* Warm-up using first frame */
    {
        FILE *f = fopen(argv[4], "rb");
        if (!f) { perror(argv[4]); failed = 1; goto cleanup; }
        size_t read_size = fread(raw, 1, in_sz, f);
        fclose(f);
        if (read_size != in_sz) {
            fprintf(stderr, "short warm-up read\n"); failed = 1; goto cleanup;
        }
        unsigned char *out = NULL; size_t out_sz = 0;
        for (int warmup = 0; warmup < 2; warmup++) {
            if (gpr_encode_fused_frame(enc, (unsigned char *)raw, in_sz, &out, &out_sz) != 0 || !out || !out_sz) {
                fprintf(stderr, "warm-up encode failed\n"); failed = 1; goto cleanup;
            }
        }
    }

    int skip_decode = 0;
    {
        const char *e = getenv("MULTI_FRAME_SKIP_DECODE");
        if (e && *e == '1') skip_decode = 1;
    }
    int skip_writes = 0;
    {
        const char *e = getenv("MULTI_FRAME_SKIP_WRITES");
        if (e && *e == '1') skip_writes = 1;
    }
    int reuse_raw = 0;
    {
        const char *e = getenv("MULTI_FRAME_REUSE_RAW");
        if (e && *e == '1') reuse_raw = 1;
    }
    fprintf(stderr, "# frame  src                   enc_ms  dec_ms  bytes      dw x dh\n");
    for (int i = 0; i < nframes; i++) {
        const char *src_path = argv[4 + i];
        if (!(reuse_raw && i > 0)) {
            FILE *f = fopen(src_path, "rb");
            if (!f || fread(raw, 1, in_sz, f) != in_sz) {
                if (f) fclose(f);
                fprintf(stderr, "read failed: %s\n", src_path); failed = 1; continue;
            }
            fclose(f);
        }

        double t0 = now_ms();
        unsigned char *out = NULL; size_t out_sz = 0;
        if (gpr_encode_fused_frame(enc, (unsigned char *)raw, in_sz, &out, &out_sz) != 0 || !out || out_sz < 13 * 4) {
            fprintf(stderr, "frame %d ENCODE failed\n", i); failed = 1; continue;
        }
        double t1 = now_ms();
        /* Compute output dims from the encoded header so we can pick the right
           pitch up front and run decode exactly once. The header is the first
           bytes of the encoded buffer (see FUSED_HEADER in fused_encode.h). */
        int dw = W, dh = H;
        if (out_sz >= 13 * 4) {
            uint32_t hdr_width    = ((uint32_t)out[ 8]) | ((uint32_t)out[ 9]<<8)
                                   | ((uint32_t)out[10]<<16) | ((uint32_t)out[11]<<24);
            uint32_t hdr_height   = ((uint32_t)out[12]) | ((uint32_t)out[13]<<8)
                                   | ((uint32_t)out[14]<<16) | ((uint32_t)out[15]<<24);
            /* FUSED_HEADER field offsets: magic=0 version=4 width=8 height=12
               pixel_format=16 quality=20 is_rggb=24 log_bits=28 prescale=32
               multi_level=36 num_bands=40 decimate=44. */
            uint32_t hdr_decimate = ((uint32_t)out[44]) | ((uint32_t)out[45]<<8)
                                   | ((uint32_t)out[46]<<16) | ((uint32_t)out[47]<<24);
            int dec_factor = (hdr_decimate < 2) ? 1 : (int)hdr_decimate;
            dw = (int)(hdr_width  / dec_factor);
            dh = (int)(hdr_height / dec_factor);
        }
        if (dw <= 0 || dh <= 0 || dw > W || dh > H) {
            fprintf(stderr, "frame %d invalid output dimensions\n", i); failed = 1; continue;
        }
        /* MULTI_FRAME_DUMP_ENCODED=1: dump the encoded buffer for byte-identity
           checks against assembly/scalar paths. Filename: <prefix>_<i>.gpr. */
        {
            const char *dump_env = getenv("MULTI_FRAME_DUMP_ENCODED");
            if (dump_env && *dump_env == '1') {
                char dump_path[512];
                snprintf(dump_path, sizeof(dump_path), "%s_%03d.gpr", prefix, i);
                if (write_buffer(dump_path, out, out_sz)) {
                    fprintf(stderr, "write failed: %s\n", dump_path); failed = 1;
                }
            }
        }

        double t2 = t1;
        if (!skip_decode) {
            int drc = gpr_decode_fused(out, out_sz, dec, (size_t)dw * 2, &dw, &dh);
            t2 = now_ms();
            if (drc != 0 || dw <= 0 || dh <= 0 || dw > W || dh > H) {
                fprintf(stderr, "frame %d DECODE rc=%d dims=%dx%d\n", i, drc, dw, dh);
                failed = 1; continue;
            }
        }

        const char *name = strrchr(src_path, '/'); name = name ? name + 1 : src_path;
        fprintf(stderr, "# %3d    %-22s %6.1f  %6.1f  %-9zu  %4d x %4d\n",
                i, name, t1 - t0, t2 - t1, out_sz, dw, dh);

        if (!skip_writes && !skip_decode) {
            /* Write decoded raw (LE u16) and demosaiced PPM */
            char path[512];
            snprintf(path, sizeof(path), "%s_%03d.raw", prefix, i);
            if (write_buffer(path, dec, (size_t)dw * dh * 2)) {
                fprintf(stderr, "write failed: %s\n", path); failed = 1;
            }
            snprintf(path, sizeof(path), "%s_%03d.ppm", prefix, i);
            if (demosaic_to_ppm(dec, dw, dh, 4, path)) {
                fprintf(stderr, "write failed: %s\n", path); failed = 1;
            }
        }
    }

cleanup:
    gpr_encode_fused_destroy(enc);
    free(raw); free(dec);
    return failed ? 1 : 0;
}
