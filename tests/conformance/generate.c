/* tests/conformance/generate.c — populate inputs/ and golden/.
 *
 * Run once at first-time setup, or when the encoder bitstream changes
 * intentionally. Re-run regenerates the golden md5s; review with
 *   git diff tests/conformance/golden/
 * before committing.
 *
 * Encoder determinism: forces FUSED_THREADS=1 so the serial path is
 * exercised (the parallel rANS path is also deterministic in practice,
 * but pinning serial removes any future threading surprise).
 *
 * Compile: see tests/conformance/build.sh.
 */

#include "common.h"

static int encode_and_write_md5(const input_def_t *in,
                                const uint16_t *raw,
                                int q,
                                const char *golden_dir) {
    unsigned char *out = NULL;
    size_t out_sz = 0;
    size_t raw_bytes = (size_t)in->width * in->height * 2;
    int rc = gpr_encode_fused((const unsigned char *)raw, raw_bytes,
                              in->width, in->height, 1 /* RGGB14 */, q,
                              &out, &out_sz);
    if (rc != 0 || !out || out_sz == 0) {
        fprintf(stderr, "  FAIL encode %s q=%d L=%d rc=%d sz=%zu\n",
                in->name, q, FUSED_WAVELET_LEVELS, rc, out_sz);
        if (out) free(out);
        return -1;
    }

    char hex[33];
    md5_hex(out, out_sz, hex);
    free(out);

    char path[512];
    golden_path(path, sizeof(path), golden_dir, in->name, q, FUSED_WAVELET_LEVELS);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "  FAIL open %s for write\n", path);
        return -1;
    }
    fprintf(f, "%s\n", hex);
    fclose(f);

    fprintf(stderr, "  OK   %-16s q=%d L=%d  enc=%zu B  md5=%s\n",
            in->name, q, FUSED_WAVELET_LEVELS, out_sz, hex);
    return 0;
}

int main(int argc, char **argv) {
    const char *base = (argc > 1) ? argv[1] : "tests/conformance";
    char inputs_dir[512], golden_dir[512];
    snprintf(inputs_dir, sizeof(inputs_dir), "%s/inputs", base);
    snprintf(golden_dir, sizeof(golden_dir), "%s/golden", base);

    configure_encoder_for_conformance();

    fprintf(stderr, "Generating conformance corpus  (wavelet levels = %d)\n",
            FUSED_WAVELET_LEVELS);
    fprintf(stderr, "  inputs: %s\n  golden: %s\n", inputs_dir, golden_dir);

    int total = 0, failed = 0;
    for (int i = 0; i < NUM_INPUTS; i++) {
        const input_def_t *in = &INPUTS[i];
        char raw_path[512];
        snprintf(raw_path, sizeof(raw_path), "%s/%s.raw", inputs_dir, in->name);
        if (ensure_input(in, raw_path) != 0) {
            fprintf(stderr, "  FAIL create input %s\n", raw_path);
            failed++;
            continue;
        }
        size_t npx = (size_t)in->width * in->height;
        uint16_t *raw = read_raw(raw_path, npx);
        if (!raw) {
            fprintf(stderr, "  FAIL read %s\n", raw_path);
            failed++;
            continue;
        }
        for (int qi = 0; qi < NUM_QUALITIES; qi++) {
            int q = QUALITIES[qi];
            total++;
            if (encode_and_write_md5(in, raw, q, golden_dir) != 0) failed++;
        }
        free(raw);
    }

    fprintf(stderr, "\n%d/%d golden md5s written\n", total - failed, total);
    return failed == 0 ? 0 : 1;
}
