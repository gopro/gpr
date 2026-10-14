/* tests/conformance/common.h — shared helpers for generate.c and check.c
 *
 * Defines:
 *   - the input frame corpus (4 synthetic patterns)
 *   - the (quality, levels) test matrix
 *   - input generators (gradient, LCG noise, edge, flat)
 *   - md5 hex helpers
 *
 * Each input is uint16 RGGB14 (or constant flat) stored little-endian as
 * raw bytes. Encoder is called via gpr_encode_fused() with pixel_format=1
 * (RGGB14). The conformance binary's FUSED_WAVELET_LEVELS macro selects
 * the runtime encoder mode it pins before every encode.
 */

#ifndef CONFORMANCE_COMMON_H
#define CONFORMANCE_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "md5.h"

/* Encoder entry point. */
extern int gpr_encode_fused(const unsigned char *raw, size_t sz,
                            int w, int h, int pf, int q,
                            unsigned char **out, size_t *out_sz);

/* Conformance mode selector. Default 2 (production multi-level path);
   pass -DFUSED_WAVELET_LEVELS=1 to exercise the single-level path. */
#ifndef FUSED_WAVELET_LEVELS
#define FUSED_WAVELET_LEVELS 2
#endif

static void configure_encoder_for_conformance(void) {
    setenv("FUSED_THREADS", "1", 1);
    if (FUSED_WAVELET_LEVELS == 1) {
        setenv("FUSED_MULTI_LEVEL", "0", 1);
        unsetenv("FUSED_WAVELET_LEVELS");
    } else {
        setenv("FUSED_MULTI_LEVEL", "1", 1);
        char levels[8];
        snprintf(levels, sizeof(levels), "%d", FUSED_WAVELET_LEVELS);
        setenv("FUSED_WAVELET_LEVELS", levels, 1);
    }
}

/* Corpus: 4 inputs × 3 qualities × 2 levels handled per-binary. */
typedef struct {
    const char *name;     /* base filename, no extension */
    int width, height;
    int pattern;          /* 0=gradient, 1=noise, 2=edge, 3=flat */
    uint32_t seed;        /* used by pattern==1 */
} input_def_t;

static const input_def_t INPUTS[] = {
    { "gradient_256",   256, 256, 0, 0 },
    { "noise_256",      256, 256, 1, 0xDEADBEEFu },
    { "edge_512x384",   512, 384, 2, 0 },
    { "flat_512x512",   512, 512, 3, 0 },
};
static const int NUM_INPUTS = (int)(sizeof(INPUTS) / sizeof(INPUTS[0]));

static const int QUALITIES[] = { 0, 3, 5 };
static const int NUM_QUALITIES = 3;

/* ----------------------- Input pattern generators ----------------------- */

/* Generate a 14-bit RGGB gradient: pixel = ((x + y) * 16) & 0x3FFF.
   Deterministic, smooth diagonal ramp. */
static void gen_gradient(uint16_t *buf, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            buf[y * w + x] = (uint16_t)(((x + y) * 16) & 0x3FFF);
        }
    }
}

/* LCG pseudo-random 14-bit values, fixed seed → byte-identical every run. */
static void gen_noise(uint16_t *buf, int w, int h, uint32_t seed) {
    uint32_t state = seed;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        state = state * 1103515245u + 12345u;
        buf[i] = (uint16_t)((state >> 4) & 0x3FFF);
    }
}

/* High-contrast vertical edge: left half = 1024, right half = 14000.
   Edge sits at column w/2. Tests wavelet edge response. */
static void gen_edge(uint16_t *buf, int w, int h) {
    int mid = w / 2;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            buf[y * w + x] = (uint16_t)((x < mid) ? 1024 : 14000);
        }
    }
}

/* Constant 8192 (mid-grey for 14-bit). Tests DC preservation. */
static void gen_flat(uint16_t *buf, int w, int h) {
    for (size_t i = 0; i < (size_t)w * h; i++) buf[i] = 8192;
}

static int write_raw(const char *path, const uint16_t *buf, size_t npx) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t wrote = fwrite(buf, 2, npx, f);
    fclose(f);
    return (wrote == npx) ? 0 : -1;
}

static uint16_t *read_raw(const char *path, size_t npx) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint16_t *buf = (uint16_t *)malloc(npx * 2);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 2, npx, f);
    fclose(f);
    if (got != npx) { free(buf); return NULL; }
    return buf;
}

/* Generate the input file at path if it doesn't exist. */
static int ensure_input(const input_def_t *in, const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return 0;  /* already exists */
    size_t npx = (size_t)in->width * in->height;
    uint16_t *buf = (uint16_t *)malloc(npx * 2);
    if (!buf) return -1;
    switch (in->pattern) {
        case 0: gen_gradient(buf, in->width, in->height); break;
        case 1: gen_noise(buf, in->width, in->height, in->seed); break;
        case 2: gen_edge(buf, in->width, in->height); break;
        case 3: gen_flat(buf, in->width, in->height); break;
        default: free(buf); return -1;
    }
    int rc = write_raw(path, buf, npx);
    free(buf);
    return rc;
}

/* ----------------------- md5 helpers ----------------------- */

static void md5_hex(const uint8_t *data, size_t len, char out_hex[33]) {
    context_md5_t ctx;
    unsigned char digest[16];
    MD5Init(&ctx);
    MD5Update(&ctx, (unsigned char *)data, (unsigned)len);
    MD5Final(digest, &ctx);
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out_hex[i * 2]     = H[(digest[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = H[ digest[i]       & 0xF];
    }
    out_hex[32] = '\0';
}

/* Path helper: "<dir>/<name>_q<q>_L<l>.md5"   (level fixed at compile time). */
static void golden_path(char *out, size_t cap, const char *dir,
                        const char *name, int q, int levels) {
    snprintf(out, cap, "%s/%s_q%d_L%d.md5", dir, name, q, levels);
}

#endif /* CONFORMANCE_COMMON_H */
