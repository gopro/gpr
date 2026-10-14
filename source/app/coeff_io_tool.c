/*
 * coeff_io_tool — single-shot encode/decode tool for codec-anchored
 * refinement experiments.
 *
 * Reads raw 16-bit Bayer (w*h*2 bytes), encodes via the fused encoder
 * with the env vars set up the same as the production ml2_q3_dec2
 * pipeline, decodes via gpr_decode_fused, and writes the decoded raw
 * to disk. The decoder will honor GPR_DUMP_COEFFS / GPR_LOAD_COEFFS
 * if those are set in the env.
 *
 * Usage:
 *   coeff_io_tool <in.raw> <w> <h> <out.raw>
 *
 * Output:
 *   - <out.raw> contains the decoded raw bayer. Dims may be half of
 *     (w, h) if GPR_ROW_DECIMATE=2 + GPR_COL_DECIMATE=2 are in the env.
 *
 * Env vars consumed:
 *   FUSED_QUALITY, FUSED_MULTI_LEVEL, FUSED_WAVELET_LEVELS,
 *   GPR_BENCH_PIXEL_FORMAT,
 *   GPR_COL_DECIMATE, GPR_ROW_DECIMATE,
 *   GPR_INCLUDE_LL, GPR_DUMP_COEFFS, GPR_LOAD_COEFFS.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include "fused_encode.h"
#include "fused_decode.h"

extern int gpr_encode_fused(const unsigned char *raw, size_t sz,
    int w, int h, int pf, int q, unsigned char **out, size_t *out_sz);

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <in.raw> <w> <h> <out.raw>\n", argv[0]);
        return 1;
    }
    const char *in_path = argv[1];
    int w = atoi(argv[2]);
    int h = atoi(argv[3]);
    const char *out_path = argv[4];

    FILE *fi = fopen(in_path, "rb");
    if (!fi) { perror(in_path); return 2; }
    size_t sz = (size_t)w * h * 2;
    unsigned char *raw = (unsigned char *)malloc(sz);
    if (!raw) { fprintf(stderr, "OOM raw\n"); return 3; }
    if (fread(raw, 1, sz, fi) != sz) { fprintf(stderr, "short read on %s\n", in_path); return 4; }
    fclose(fi);

    const char *q_env = getenv("FUSED_QUALITY");
    int q = q_env && *q_env ? atoi(q_env) : 3;

    /* Use the proper encoder create/frame API (matches test_fused_decode_roundtrip).
       Declarations come from fused_encode.h. */
    struct timespec t_enc_start, t_enc_end, t_dec_start, t_dec_end;
    clock_gettime(CLOCK_MONOTONIC, &t_enc_start);
    const char *pf_env = getenv("GPR_BENCH_PIXEL_FORMAT");
    int pf = pf_env && *pf_env ? atoi(pf_env) : 1;
    FUSED_ENCODER *eh = gpr_encode_fused_create(w, h, pf, q);
    if (!eh) { fprintf(stderr, "encoder create failed\n"); return 5; }
    unsigned char *enc = NULL; size_t enc_sz = 0;
    int rc = gpr_encode_fused_frame(eh, raw, sz, &enc, &enc_sz);
    if (rc != 0) {
        fprintf(stderr, "encode failed rc=%d\n", rc);
        return 5;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_enc_end);
    double enc_ms = (t_enc_end.tv_sec - t_enc_start.tv_sec) * 1000.0
                  + (t_enc_end.tv_nsec - t_enc_start.tv_nsec) / 1e6;
    fprintf(stderr, "ENCODE: %zu bytes in %.1f ms\n", enc_sz, enc_ms);

    /* Optional: save the encoded .gpr to disk so it can be consumed by
       editable-raw workflows (decode + raw editor). */
    const char *save_path = getenv("GPR_SAVE_TO");
    if (save_path && *save_path) {
        FILE *gf = fopen(save_path, "wb");
        if (gf) {
            if (fwrite(enc, 1, enc_sz, gf) != enc_sz) {
                fprintf(stderr, "GPR_SAVE_TO: short write on %s\n", save_path);
            } else {
                fprintf(stderr, "GPR_SAVE_TO: wrote %zu bytes to %s\n", enc_sz, save_path);
            }
            fclose(gf);
        } else {
            fprintf(stderr, "GPR_SAVE_TO: fopen(%s) failed\n", save_path);
        }
    }

    /* Two-step decode: first call discovers actual output dims (may be
       half-res with decimation on); second call uses the correct pitch. */
    clock_gettime(CLOCK_MONOTONIC, &t_dec_start);
    uint16_t *recon = (uint16_t *)calloc(1, sz);
    if (!recon) { fprintf(stderr, "OOM recon\n"); return 6; }
    int out_w = 0, out_h = 0;
    int drc = gpr_decode_fused(enc, enc_sz, recon, (size_t)w * 2, &out_w, &out_h);
    if (drc != 0) {
        fprintf(stderr, "decode (step 1) failed rc=%d\n", drc);
        return 7;
    }
    if (out_w != w || out_h != h) {
        /* Re-decode with correct half-res pitch. */
        memset(recon, 0, sz);
        drc = gpr_decode_fused(enc, enc_sz, recon, (size_t)out_w * 2, &out_w, &out_h);
        if (drc != 0) {
            fprintf(stderr, "decode (step 2 re-pitch) failed rc=%d\n", drc);
            return 7;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t_dec_end);
    double dec_ms = (t_dec_end.tv_sec - t_dec_start.tv_sec) * 1000.0
                  + (t_dec_end.tv_nsec - t_dec_start.tv_nsec) / 1e6;
    fprintf(stderr, "DECODE: %dx%d output in %.1f ms\n", out_w, out_h, dec_ms);

    FILE *fo = fopen(out_path, "wb");
    if (!fo) { perror(out_path); return 8; }
    size_t out_bytes = (size_t)out_w * out_h * 2;
    if (fwrite(recon, 1, out_bytes, fo) != out_bytes) {
        fprintf(stderr, "short write on %s\n", out_path);
        return 9;
    }
    fclose(fo);
    free(recon);
    gpr_encode_fused_destroy(eh);
    free(raw);
    return 0;
}
