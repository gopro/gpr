/* tests/conformance/check.c — verify bitstream stability against golden md5s.
 *
 * Exits 0 if every (input, quality, levels) encode matches its golden md5.
 * Exits 1 with a per-test diff report on any mismatch.
 *
 * Run on every PR / in CI. To intentionally update the goldens after an
 * encoder change, re-run `generate` and review the diff under
 * tests/conformance/golden/.
 *
 * Encoder determinism: forces FUSED_THREADS=1 (matches generate.c).
 *
 * Compile: see tests/conformance/build.sh.
 */

#include "common.h"

static int read_golden(const char *path, char out_hex[33]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[64];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (got < 32) return -1;
    buf[got] = '\0';
    /* Copy first 32 hex chars; ignore any trailing newline/whitespace. */
    for (int i = 0; i < 32; i++) {
        char c = buf[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return -1;
        out_hex[i] = c;
    }
    out_hex[32] = '\0';
    return 0;
}

static int check_one(const input_def_t *in,
                     const uint16_t *raw,
                     int q,
                     const char *golden_dir,
                     int *out_match,
                     char actual_hex[33],
                     char golden_hex[33]) {
    *out_match = 0;
    unsigned char *out = NULL;
    size_t out_sz = 0;
    size_t raw_bytes = (size_t)in->width * in->height * 2;
    int rc = gpr_encode_fused((const unsigned char *)raw, raw_bytes,
                              in->width, in->height, 1 /* RGGB14 */, q,
                              &out, &out_sz);
    if (rc != 0 || !out || out_sz == 0) {
        if (out) free(out);
        snprintf(actual_hex, 33, "<encode-fail>");
        return -1;
    }
    md5_hex(out, out_sz, actual_hex);
    free(out);

    char path[512];
    golden_path(path, sizeof(path), golden_dir, in->name, q, FUSED_WAVELET_LEVELS);
    if (read_golden(path, golden_hex) != 0) {
        snprintf(golden_hex, 33, "<missing-golden>");
        return -1;
    }
    *out_match = (memcmp(actual_hex, golden_hex, 32) == 0);
    return 0;
}

int main(int argc, char **argv) {
    const char *base = (argc > 1) ? argv[1] : "tests/conformance";
    char inputs_dir[512], golden_dir[512];
    snprintf(inputs_dir, sizeof(inputs_dir), "%s/inputs", base);
    snprintf(golden_dir, sizeof(golden_dir), "%s/golden", base);

    configure_encoder_for_conformance();

    fprintf(stderr, "Conformance check  (wavelet levels = %d)\n",
            FUSED_WAVELET_LEVELS);
    fprintf(stderr, "  inputs: %s\n  golden: %s\n", inputs_dir, golden_dir);

    int total = 0, mismatches = 0, errors = 0;
    for (int i = 0; i < NUM_INPUTS; i++) {
        const input_def_t *in = &INPUTS[i];
        char raw_path[512];
        snprintf(raw_path, sizeof(raw_path), "%s/%s.raw", inputs_dir, in->name);
        size_t npx = (size_t)in->width * in->height;
        uint16_t *raw = read_raw(raw_path, npx);
        if (!raw) {
            fprintf(stderr, "  FAIL read input %s — run generate first\n", raw_path);
            errors++;
            continue;
        }
        for (int qi = 0; qi < NUM_QUALITIES; qi++) {
            int q = QUALITIES[qi];
            int match = 0;
            char actual[33], golden[33];
            int rc = check_one(in, raw, q, golden_dir, &match, actual, golden);
            total++;
            if (rc != 0) {
                fprintf(stderr, "  ERR  %-16s q=%d L=%d  actual=%s golden=%s\n",
                        in->name, q, FUSED_WAVELET_LEVELS, actual, golden);
                errors++;
            } else if (!match) {
                fprintf(stderr, "  DIFF %-16s q=%d L=%d  actual=%s  golden=%s\n",
                        in->name, q, FUSED_WAVELET_LEVELS, actual, golden);
                mismatches++;
            } else {
                fprintf(stderr, "  OK   %-16s q=%d L=%d  md5=%s\n",
                        in->name, q, FUSED_WAVELET_LEVELS, actual);
            }
        }
        free(raw);
    }

    int failed = mismatches + errors;
    fprintf(stderr, "\n%d/%d matched   (%d diffs, %d errors)\n",
            total - failed, total, mismatches, errors);
    if (failed != 0) {
        fprintf(stderr,
                "\nbitstream regression detected. If this is intentional,\n"
                "re-run the generate binary and commit the updated golden files.\n");
    }
    return failed == 0 ? 0 : 1;
}
