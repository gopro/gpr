/* test_fused_decode_roundtrip — single-frame encode → decode → PSNR.
 *
 * Sibling to test_fused_roundtrip.c which only validates the band-level
 * rANS roundtrip. This one runs the FULL pipeline:
 *   1. Read raw Bayer
 *   2. Encode via gpr_encode_fused_frame (respects GPR_* env flags)
 *   3. Decode via gpr_decode_fused
 *   4. If dims match source (no decimation), compute PSNR
 *   5. Optionally write decoded Bayer to disk for downstream rendering
 *
 * Crucial gotcha (one I hit and burned several hours on): if the encoder
 * applied channel-space decimation, the decoded dims are smaller than the
 * source dims. The first decode call uses the source pitch which spaces
 * rows TOO FAR APART in the output buffer — the saved file would be a
 * mess of valid row + zero padding. The fix is to re-decode with the
 * correct pitch once the output dims are known. We do that here.
 *
 * Build:
 *   gcc -O2 source/app/test_fused_decode_roundtrip.c \
 *       build/source/lib/vc5_decoder/libvc5_decoder.a \
 *       build/source/lib/vc5_encoder/libvc5_encoder.a \
 *       build/source/lib/vc5_common/libvc5_common.a \
 *       build/source/lib/common/libcommon.a \
 *       -lpthread -lm -o /tmp/roundtrip
 *
 * Usage:
 *   roundtrip raw W H [decoded.raw]
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

typedef struct FUSED_ENCODER FUSED_ENCODER;
extern FUSED_ENCODER *gpr_encode_fused_create(int w, int h, int pf, int q);
extern int gpr_encode_fused_frame(FUSED_ENCODER *ctx, const unsigned char *raw,
                                   size_t sz, unsigned char **out, size_t *out_sz);
extern void gpr_encode_fused_destroy(FUSED_ENCODER *ctx);

extern int gpr_decode_fused(const unsigned char *enc, size_t enc_size,
                             uint16_t *bayer_out, size_t bayer_pitch_bytes,
                             int *out_width, int *out_height);

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s raw W H [decoded.raw]\n", argv[0]); return 1; }
    int w = atoi(argv[2]), h = atoi(argv[3]);
    size_t sz = (size_t)w * h * 2;
    uint16_t *raw = malloc(sz);
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(raw, 1, sz, f) != sz) { fprintf(stderr, "read fail\n"); return 1; }
    fclose(f);

    /* Encode (FUSED_QUALITY env overrides default q=3) */
    const char *q_env = getenv("FUSED_QUALITY");
    int q = (q_env && *q_env) ? atoi(q_env) : 3;
    const char *pf_env = getenv("GPR_BENCH_PIXEL_FORMAT");
    int pf = (pf_env && *pf_env) ? atoi(pf_env) : 1;
    FUSED_ENCODER *enc = gpr_encode_fused_create(w, h, pf, q);
    if (!enc) { fprintf(stderr, "create fail\n"); return 1; }
    double t0 = now_ms();
    unsigned char *out = NULL; size_t out_sz = 0;
    int rc = gpr_encode_fused_frame(enc, (unsigned char *)raw, sz, &out, &out_sz);
    double t_enc = now_ms() - t0;
    if (rc != 0) { fprintf(stderr, "encode rc=%d\n", rc); return 2; }
    fprintf(stderr, "ENCODE: %zu bytes in %.1f ms\n", out_sz, t_enc);

    /* Optionally save the encoded bitstream (the real .gpr) — set
       GPR_SAVE_ENC_TO=<path> when the caller wants the encoder output
       itself, not the roundtrip decoded bayer at argv[4]. Used by the
       Pi-to-Mac UPRESABLE bench. */
    const char *enc_save = getenv("GPR_SAVE_ENC_TO");
    if (enc_save && *enc_save) {
        FILE *eg = fopen(enc_save, "wb");
        if (eg) {
            fwrite(out, 1, out_sz, eg);
            fclose(eg);
        }
    }

    /* Skip the decode pass when caller only wants encoded output and
       hasn't asked for a decoded raw — saves ~75% of per-frame time
       on the Pi capture-only path. */
    const char *skip_dec = getenv("GPR_SKIP_DECODE");
    if (skip_dec && skip_dec[0] == '1' && argc < 5) {
        gpr_encode_fused_destroy(enc);
        free(raw);
        return 0;
    }

    /* Decode */
    int dw = 0, dh = 0;
    /* First peek-decode to get dimensions (allocate generous buffer). */
    size_t dec_cap = (size_t)w * h * 2;
    uint16_t *dec = calloc(1, dec_cap);
    if (!dec) { fprintf(stderr, "decoded buf alloc fail\n"); return 3; }
    double t1 = now_ms();
    /* Use w*2 byte pitch as a reasonable max — decoder writes (out_w*2) bytes
       per output bayer row. We don't know output dims yet, but the buffer is
       sized for the full input so this is safe. We'll pass a pitch that
       matches the EXPECTED output width (from header). For first call we
       pass w*2 and rely on decoder writing into the start of buf. To safely
       handle smaller outputs, we make the pitch tight. The decoder accepts
       any pitch ≥ out_w*2. */
    /* Simplest: pass the encoded header's width as the pitch via two-step
       approach. Use a tight loop: try with original width pitch first, then
       reallocate if dims differ. */
    int drc = gpr_decode_fused(out, out_sz, dec, (size_t)w * 2, &dw, &dh);
    double t_dec = now_ms() - t1;
    if (drc != 0) {
        fprintf(stderr, "DECODE failed: rc=%d\n", drc);
        return 4;
    }
    fprintf(stderr, "DECODE: %dx%d in %.1f ms\n", dw, dh, t_dec);

    /* Re-decode with correct pitch if dimensions differ. */
    if (dw != w || dh != h) {
        size_t dec_sz = (size_t)dw * dh * 2;
        memset(dec, 0, dec_cap);
        drc = gpr_decode_fused(out, out_sz, dec, (size_t)dw * 2, &dw, &dh);
        if (drc != 0) {
            fprintf(stderr, "DECODE (re-pitch) failed: rc=%d\n", drc);
            return 4;
        }
    }

    /* Quality metrics: PSNR vs source (only valid if dims match). */
    if (dw == w && dh == h) {
        double sse = 0;
        size_t npx = (size_t)w * h;
        for (size_t i = 0; i < npx; i++) {
            double d = (double)raw[i] - (double)dec[i];
            sse += d * d;
        }
        double mse = sse / npx;
        int log_bits = (pf >= 4) ? 16 : 14;
        double peak = (double)((1 << log_bits) - 1);
        double psnr = 10.0 * log10((peak * peak) / (mse + 1e-12));
        fprintf(stderr, "PSNR%d (full-res): %.2f dB  mse=%.1f\n",
                log_bits, psnr, mse);
    } else {
        fprintf(stderr, "decoded dims %dx%d differ from source %dx%d "
                "(decimation in effect — full-res PSNR not meaningful; "
                "compare against decimated source)\n", dw, dh, w, h);
    }

    /* Sanity histogram: min/max/mean of decoded */
    size_t npx_d = (size_t)dw * dh;
    uint64_t sum = 0; uint16_t mn = 65535, mx = 0;
    for (size_t i = 0; i < npx_d; i++) {
        if (dec[i] < mn) mn = dec[i];
        if (dec[i] > mx) mx = dec[i];
        sum += dec[i];
    }
    double mean = (double)sum / npx_d;
    fprintf(stderr, "Decoded stats: min=%u  max=%u  mean=%.1f  npx=%zu\n",
            mn, mx, mean, npx_d);

    if (argc >= 5) {
        FILE *g = fopen(argv[4], "wb");
        if (g) {
            fwrite(dec, 2, npx_d, g);
            fclose(g);
            fprintf(stderr, "wrote decoded to %s (%zu bytes)\n",
                    argv[4], npx_d * 2);
        }
    }

    gpr_encode_fused_destroy(enc);
    free(raw); free(dec);
    return 0;
}
