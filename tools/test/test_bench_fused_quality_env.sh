#!/usr/bin/env bash
# Verify bench_fused honors FUSED_QUALITY instead of silently hard-coding q=3.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO/build}"
BENCH="${BENCH:-$BUILD_DIR/source/app/bench_fused/bench_fused}"
# A caller-provided WORK is a parent directory, never a cleanup target.
WORK_ROOT="${WORK:-${TMPDIR:-/tmp}}"

if [ ! -x "$BENCH" ]; then
    echo "ERROR: bench_fused not built at $BENCH" >&2
    exit 2
fi

mkdir -p -- "$WORK_ROOT"
WORK=$(mktemp -d "$WORK_ROOT/gpr_bench_quality_smoke.XXXXXX")
RAW="$WORK/input.raw"
Q3="$WORK/q3.gpr"
Q11="$WORK/q11.gpr"
PF5="$WORK/pf5.gpr"

python3 - "$RAW" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
w, h = 512, 512
with path.open("wb") as f:
    for y in range(h):
        row = bytearray(w * 2)
        for x in range(w):
            v = (2048 + x * 9 + y * 13 + ((x * y) & 255)) & 0x3FFF
            row[2 * x] = v & 0xFF
            row[2 * x + 1] = (v >> 8) & 0xFF
        f.write(row)
PY

GPR_BENCH_DUMP="$Q3" \
GPR_INCLUDE_LL=1 \
FUSED_MULTI_LEVEL=1 \
FUSED_WAVELET_LEVELS=2 \
GPR_COL_DECIMATE=2 \
GPR_ROW_DECIMATE=2 \
FUSED_QUALITY=3 \
"$BENCH" "$RAW" 512 512 1 >/dev/null

GPR_BENCH_DUMP="$Q11" \
GPR_INCLUDE_LL=1 \
FUSED_MULTI_LEVEL=1 \
FUSED_WAVELET_LEVELS=2 \
GPR_COL_DECIMATE=2 \
GPR_ROW_DECIMATE=2 \
FUSED_QUALITY=11 \
"$BENCH" "$RAW" 512 512 1 >/dev/null

python3 - "$Q3" "$Q11" <<'PY'
import sys
import struct
from pathlib import Path

q3 = Path(sys.argv[1]).stat().st_size
q11 = Path(sys.argv[2]).stat().st_size
if q3 == q11:
    raise SystemExit(f"FUSED_QUALITY did not change payload size: {q3}")

header = struct.unpack("<12I", Path(sys.argv[1]).read_bytes()[:48])
if header[4] != 4:
    raise SystemExit(f"default GPR_BENCH_PIXEL_FORMAT did not reach encoded header: {header[4]}")
PY

GPR_BENCH_DUMP="$PF5" \
GPR_INCLUDE_LL=1 \
FUSED_MULTI_LEVEL=1 \
FUSED_WAVELET_LEVELS=2 \
GPR_COL_DECIMATE=2 \
GPR_ROW_DECIMATE=2 \
GPR_BENCH_PIXEL_FORMAT=5 \
"$BENCH" "$RAW" 512 512 1 >/dev/null

python3 - "$PF5" <<'PY'
import struct
import sys
from pathlib import Path

header = struct.unpack("<12I", Path(sys.argv[1]).read_bytes()[:48])
if header[4] != 5:
    raise SystemExit(f"GPR_BENCH_PIXEL_FORMAT did not reach encoded header: {header[4]}")
PY

if FUSED_QUALITY=99 "$BENCH" "$RAW" 512 512 1 >/dev/null 2>"$WORK/invalid.err"; then
    echo "ERROR: invalid FUSED_QUALITY unexpectedly passed" >&2
    exit 1
fi
grep -q "invalid FUSED_QUALITY" "$WORK/invalid.err"

if GPR_BENCH_PIXEL_FORMAT=99 "$BENCH" "$RAW" 512 512 1 >/dev/null 2>"$WORK/invalid_pf.err"; then
    echo "ERROR: invalid GPR_BENCH_PIXEL_FORMAT unexpectedly passed" >&2
    exit 1
fi
grep -q "invalid GPR_BENCH_PIXEL_FORMAT" "$WORK/invalid_pf.err"

rm -rf -- "$WORK"
echo "test_bench_fused_quality_env: PASS"
