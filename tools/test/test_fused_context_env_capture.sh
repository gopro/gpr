#!/usr/bin/env bash
# Verify fused encoder mode flags are captured at context creation.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build}"
# A caller-provided WORK is a parent directory, never a cleanup target.
WORK_ROOT="${WORK:-${TMPDIR:-/tmp}}"

ENCODER_LIB="$BUILD_DIR/source/lib/vc5_encoder/libvc5_encoder.a"
COMMON_LIB="$BUILD_DIR/source/lib/vc5_common/libvc5_common.a"

if [ ! -f "$ENCODER_LIB" ] || [ ! -f "$COMMON_LIB" ]; then
    echo "ERROR: fused encoder libraries not built under $BUILD_DIR" >&2
    exit 2
fi

mkdir -p -- "$WORK_ROOT"
WORK=$(mktemp -d "$WORK_ROOT/gpr_context_env_capture.XXXXXX")

python3 - "$WORK/input.raw" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
w, h = 256, 256
with path.open("wb") as f:
    for y in range(h):
        row = bytearray(w * 2)
        for x in range(w):
            v = (1024 + x * 11 + y * 17 + ((x ^ y) & 127)) & 0x3FFF
            row[2 * x] = v & 0xFF
            row[2 * x + 1] = (v >> 8) & 0xFF
        f.write(row)
PY

cat >"$WORK/context_env_capture.c" <<'C'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FUSED_ENCODER FUSED_ENCODER;
extern FUSED_ENCODER *gpr_encode_fused_create(int w, int h, int pf, int q);
extern int gpr_encode_fused_frame(FUSED_ENCODER *ctx, const unsigned char *raw,
                                  size_t sz, unsigned char **out, size_t *out_sz);
extern void gpr_encode_fused_destroy(FUSED_ENCODER *ctx);

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t quality;
    uint32_t is_rggb;
    uint32_t log_bits;
    uint32_t prescale;
    uint32_t multi_level;
    uint32_t num_bands;
    uint32_t decimate;
} FUSED_HEADER;

static unsigned char *read_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) return NULL;
    long n = ftell(f);
    if (n <= 0) return NULL;
    rewind(f);
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) return NULL;
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return NULL;
    fclose(f);
    *size_out = (size_t)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s input.raw\n", argv[0]);
        return 2;
    }
    size_t raw_size = 0;
    unsigned char *raw = read_file(argv[1], &raw_size);
    if (!raw) {
        fprintf(stderr, "failed to read raw\n");
        return 3;
    }

    setenv("FUSED_MULTI_LEVEL", "1", 1);
    setenv("FUSED_WAVELET_LEVELS", "2", 1);
    setenv("GPR_INCLUDE_LL", "1", 1);
    setenv("GPR_ROW_DECIMATE", "2", 1);
    setenv("GPR_COL_DECIMATE", "2", 1);

    FUSED_ENCODER *ctx = gpr_encode_fused_create(256, 256, 1, 3);
    if (!ctx) {
        fprintf(stderr, "create failed\n");
        return 4;
    }

    unsetenv("GPR_ROW_DECIMATE");
    unsetenv("GPR_COL_DECIMATE");

    unsigned char *out = NULL;
    size_t out_size = 0;
    int rc = gpr_encode_fused_frame(ctx, raw, raw_size, &out, &out_size);
    if (rc != 0 || out_size < sizeof(FUSED_HEADER)) {
        fprintf(stderr, "encode failed rc=%d size=%zu\n", rc, out_size);
        return 5;
    }

    FUSED_HEADER hdr;
    memcpy(&hdr, out, sizeof(hdr));
    if (hdr.decimate != 2) {
        fprintf(stderr, "decimate header drifted after env mutation: %u\n", hdr.decimate);
        return 6;
    }
    if (hdr.multi_level != 2 || hdr.num_bands != 28) {
        fprintf(stderr, "unexpected multilevel header: levels=%u bands=%u\n",
                hdr.multi_level, hdr.num_bands);
        return 7;
    }

    gpr_encode_fused_destroy(ctx);
    free(raw);
    return 0;
}
C

"${CC:-cc}" -O2 -o "$WORK/context_env_capture" \
    "$WORK/context_env_capture.c" \
    "$ENCODER_LIB" "$COMMON_LIB" -lpthread -lm

"$WORK/context_env_capture" "$WORK/input.raw"

rm -rf -- "$WORK"
echo "test_fused_context_env_capture: PASS"
