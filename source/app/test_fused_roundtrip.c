/*
 * test_fused_roundtrip — verify the fused encoder's wrapper format
 * round-trips at the band level.
 *
 * Stage 1: encode a frame, parse the wrapper header, decode each rANS
 * band stream back to int32 coefficients via jans_decode_band_x4, and
 * verify each band decodes without error. This validates the encoder
 * output structure + the rANS roundtrip, but does NOT do the inverse
 * wavelet transform yet (that's Stage 2).
 *
 * Usage:
 *   test_fused_roundtrip <raw_file> <width> <height>
 *   test_fused_roundtrip                    # uses synthetic gradient
 *
 * Exit 0 on success, nonzero on any decode mismatch.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fused_encode.h"
#include "ans_joint.h"
#include "fused_decode.h"

#include <math.h>

extern int gpr_encode_fused(const unsigned char *raw, size_t sz,
    int w, int h, int pf, int q, unsigned char **out, size_t *out_sz);

/* Compute PSNR (dB) between two uint16 Bayer planes. log_max is the
   maximum valid value (e.g. 16383 for 14-bit). */
static double psnr_uint16(const uint16_t *a, const uint16_t *b,
                          int width, int height, int log_max) {
    double sse = 0.0;
    size_t n = (size_t)width * height;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        sse += d * d;
    }
    if (sse <= 0.0) return 99.0;  /* identical */
    double mse = sse / (double)n;
    double peak = (double)log_max;
    return 10.0 * log10((peak * peak) / mse);
}

static unsigned char *make_synthetic(int w, int h, uint32_t seed) {
    size_t sz = (size_t)w * h * 2;
    unsigned char *d = (unsigned char *)malloc(sz);
    uint16_t *p = (uint16_t *)d;
    uint32_t x = seed;
    for (int y = 0; y < h; y++) {
        for (int xc = 0; xc < w; xc++) {
            x = x * 1103515245u + 12345u;
            /* Smooth gradient with mild noise — compresses realistically. */
            int v = 2000 + ((xc * 12000) / w) + (((int)(x >> 24)) & 0xFF) - 128;
            if (v < 0) v = 0; if (v > 16383) v = 16383;
            p[y * w + xc] = (uint16_t)v;
        }
    }
    return d;
}

/* Layout-aware sizes for a band by slot index (encoder writes in this order).
   Multi-level: 7 or 10 slots per channel × 4 channels.
   Single-level: 3 slots per channel × 4 channels. */
typedef struct { int w, h; const char *name; } BAND_SHAPE;

static int compute_band_shapes(const FUSED_HEADER *hdr, BAND_SHAPE *out) {
    int ch_w = hdr->width / 2;
    int ch_h = hdr->height / 2;
    int bw1 = ch_w / 2, bh1 = ch_h / 2;
    int bw2 = bw1 / 2, bh2 = bh1 / 2;
    int bw3 = bw1 / 4, bh3 = bh1 / 4;
    int idx = 0;
    if (hdr->multi_level == 2) {
        for (int ch = 0; ch < 4; ch++) {
            const char *cn[] = {"GS","RG","BG","GD"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "LH1"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "HL1"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "HH1"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "LH2"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "HL2"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "HH2"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "LL2"};
            (void)cn;
        }
    } else if (hdr->multi_level == 3) {
        for (int ch = 0; ch < 4; ch++) {
            const char *cn[] = {"GS","RG","BG","GD"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "LH1"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "HL1"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "HH1"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "LH2"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "HL2"};
            out[idx++] = (BAND_SHAPE){bw2, bh2, "HH2"};
            out[idx++] = (BAND_SHAPE){bw3, bh3, "LH3"};
            out[idx++] = (BAND_SHAPE){bw3, bh3, "HL3"};
            out[idx++] = (BAND_SHAPE){bw3, bh3, "HH3"};
            out[idx++] = (BAND_SHAPE){bw3, bh3, "LL3"};
            (void)cn;
        }
    } else {
        for (int ch = 0; ch < 4; ch++) {
            out[idx++] = (BAND_SHAPE){bw1, bh1, "LH"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "HL"};
            out[idx++] = (BAND_SHAPE){bw1, bh1, "HH"};
        }
    }
    return idx;
}

int main(int argc, char **argv) {
    int w, h;
    unsigned char *raw;
    if (argc >= 4) {
        w = atoi(argv[2]); h = atoi(argv[3]);
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        size_t sz = (size_t)w * h * 2;
        raw = (unsigned char *)malloc(sz);
        if (fread(raw, 1, sz, f) != sz) { fprintf(stderr, "short read\n"); return 1; }
        fclose(f);
    } else {
        w = 1024; h = 768;
        raw = make_synthetic(w, h, 0xABCD0042u);
    }

    int total_failures = 0;
    for (int ml = 0; ml < 2; ml++) {
        setenv("FUSED_MULTI_LEVEL", ml ? "1" : "0", 1);

        unsigned char *enc = NULL; size_t enc_sz = 0;
        int rc = gpr_encode_fused(raw, (size_t)w * h * 2, w, h, 1, 3, &enc, &enc_sz);
        if (rc != 0) { fprintf(stderr, "encode failed rc=%d\n", rc); return 1; }

        /* Parse header */
        if (enc_sz < sizeof(FUSED_HEADER)) {
            fprintf(stderr, "FAIL %s: output too small for header\n",
                    ml ? "multi" : "single");
            total_failures++; free(enc); continue;
        }
        FUSED_HEADER hdr;
        memcpy(&hdr, enc, sizeof(hdr));
        if (hdr.magic != FUSED_MAGIC) {
            fprintf(stderr, "FAIL %s: bad magic 0x%08x\n", ml ? "multi" : "single",
                    hdr.magic);
            total_failures++; free(enc); continue;
        }
        if (hdr.version != FUSED_VERSION) {
            fprintf(stderr, "FAIL %s: bad version %u\n", ml ? "multi" : "single",
                    hdr.version);
            total_failures++; free(enc); continue;
        }
        if (hdr.width != (uint32_t)w || hdr.height != (uint32_t)h) {
            fprintf(stderr, "FAIL %s: dims mismatch\n", ml ? "multi" : "single");
            total_failures++; free(enc); continue;
        }
        if (!ml && hdr.multi_level != 0) {
            fprintf(stderr, "FAIL single: multi_level=%u expected 0\n",
                    hdr.multi_level);
            total_failures++; free(enc); continue;
        }
        if (ml && hdr.multi_level != 2 && hdr.multi_level != 3) {
            fprintf(stderr, "FAIL multi: multi_level=%u expected depth 2 or 3\n",
                    hdr.multi_level);
            total_failures++; free(enc); continue;
        }
        int expected_bands = ml ? ((hdr.multi_level == 2) ? 28 : 40) : 12;
        if ((int)hdr.num_bands != expected_bands) {
            fprintf(stderr, "FAIL %s: num_bands=%u expected %d\n",
                    ml ? "multi" : "single", hdr.num_bands, expected_bands);
            total_failures++; free(enc); continue;
        }

        /* Read band-size table */
        size_t off = sizeof(FUSED_HEADER);
        uint32_t *band_sizes = (uint32_t *)malloc(sizeof(uint32_t) * hdr.num_bands);
        memcpy(band_sizes, enc + off, sizeof(uint32_t) * hdr.num_bands);
        off += sizeof(uint32_t) * hdr.num_bands;

        BAND_SHAPE shapes[40];
        int n_shapes = compute_band_shapes(&hdr, shapes);
        if (n_shapes != (int)hdr.num_bands) {
            fprintf(stderr, "FAIL %s: shape count mismatch %d vs %u\n",
                    ml ? "multi" : "single", n_shapes, hdr.num_bands);
            total_failures++; free(band_sizes); free(enc); continue;
        }

        /* Decode each band */
        int band_failures = 0;
        size_t total_band_bytes = 0;
        for (uint32_t i = 0; i < hdr.num_bands; i++) {
            uint32_t sz = band_sizes[i];
            total_band_bytes += sz;
            if (off + sz > enc_sz) {
                fprintf(stderr, "FAIL %s band %u: extends past end\n",
                        ml ? "multi" : "single", i);
                band_failures++; break;
            }
            int bw = shapes[i].w, bh = shapes[i].h;
            int32_t *coeffs = (int32_t *)malloc((size_t)bw * bh * sizeof(int32_t));
            int drc = jans_decode_band_x4(enc + off, sz,
                                           coeffs, bw, bh, bw * sizeof(int32_t));
            if (drc != 0) {
                fprintf(stderr, "FAIL %s band %u (%s, %dx%d, %u bytes): jans rc=%d\n",
                        ml ? "multi" : "single", i, shapes[i].name, bw, bh, sz, drc);
                band_failures++;
            }
            free(coeffs);
            off += sz;
        }
        if (band_failures == 0) {
            printf("PASS %s: %u bands decoded OK (%.1f KB total band data, "
                   "%zu bytes header+manifest)\n",
                   ml ? "multi" : "single", hdr.num_bands,
                   total_band_bytes / 1024.0,
                   sizeof(FUSED_HEADER) + sizeof(uint32_t) * hdr.num_bands);
        }
        total_failures += band_failures;
        free(band_sizes); free(enc);
    }

    /* ---- Full pixel-level multi-level roundtrip via gpr_decode_fused ---- */
    {
        setenv("FUSED_MULTI_LEVEL", "1", 1);
        unsigned char *enc = NULL; size_t enc_sz = 0;
        int rc = gpr_encode_fused(raw, (size_t)w * h * 2, w, h, 1, 3, &enc, &enc_sz);
        if (rc != 0) {
            fprintf(stderr, "FAIL pixel roundtrip: encode rc=%d\n", rc);
            free(raw); return 1;
        }
        uint16_t *recon = (uint16_t *)malloc((size_t)w * h * 2);
        int out_w = 0, out_h = 0;
        int drc = gpr_decode_fused(enc, enc_sz, recon, (size_t)w * 2, &out_w, &out_h);
        if (drc != 0) {
            fprintf(stderr, "FAIL pixel roundtrip: decode rc=%d\n", drc);
            free(enc); free(recon); free(raw); return 1;
        }
        if (out_w != w || out_h != h) {
            fprintf(stderr, "FAIL pixel roundtrip: dim mismatch (%dx%d vs %dx%d)\n",
                    out_w, out_h, w, h);
            free(enc); free(recon); free(raw); return 1;
        }
        double psnr = psnr_uint16((const uint16_t *)raw, recon, w, h, 16383);
        printf("PIXEL ROUNDTRIP %dx%d multi-level q=3: PSNR = %.2f dB "
               "(encoded %.1f KB)\n",
               w, h, psnr, enc_sz / 1024.0);
        /* Regression floor: synthetic gradients at this size routinely
           hit 45+ dB after the bottom-edge fixes. Anything below 35 dB
           means a real regression — bottom-edge underrun, scaling, or
           rANS overflow are likely. */
        const double psnr_floor = 35.0;
        if (psnr < psnr_floor) {
            fprintf(stderr, "FAIL pixel roundtrip: PSNR %.2f below %.1f floor\n",
                    psnr, psnr_floor);
            total_failures++;
        }
        free(enc); free(recon);
    }

    free(raw);
    if (total_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d band(s) failed to decode\n", total_failures);
    return 1;
}
