#!/usr/bin/env bash
# Exercise malformed seeds and a freshly encoded current-format GVID.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
export BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/gpr-fuzz-smoke.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
export GPR_FUZZ_BUILD_DIR="$WORK/bin"
mkdir -p "$WORK/corpus" "$WORK/crashes"
cp "$ROOT"/source/app/fuzz_decoder/corpus/*.gvid "$WORK/corpus/"
"${PYTHON_BIN:-python3}" - "$WORK/input.raw" <<'PY'
import struct
import sys
from pathlib import Path
samples = [(2048 + x * 9 + y * 13) & 4095 for y in range(128) for x in range(256)]
Path(sys.argv[1]).write_bytes(struct.pack('<32768H', *samples))
PY
GPR_INCLUDE_LL=1 FUSED_MULTI_LEVEL=0 FUSED_THREADS=1 \
  GPR_COL_DECIMATE=1 GPR_ROW_DECIMATE=1 GPR_BENCH_PIXEL_FORMAT=0 \
  GPR_BENCH_GVID="$WORK/corpus/current.gvid" \
  "$BUILD_DIR/source/app/bench_fused/bench_fused" "$WORK/input.raw" 256 128 2
"${PYTHON_BIN:-python3}" - "$WORK/corpus/current.gvid" <<'PY'
import struct
import sys
from pathlib import Path
data = Path(sys.argv[1]).read_bytes()
assert data[:4] == b'GVID' and struct.unpack_from('<I', data, 28)[0] == 2
assert data[48:52] == b'FUSD'
PY
bash "$ROOT/source/app/fuzz_decoder/build.sh" standalone
"$GPR_FUZZ_BUILD_DIR/fuzz_decoder_standalone" "$WORK"/corpus/*.gvid
if [ "${GPR_FUZZ_SECONDS:-0}" -gt 0 ]; then
  bash "$ROOT/source/app/fuzz_decoder/build.sh" fuzzer
  for name in fuzz_decoder fuzz_ans; do
    "$GPR_FUZZ_BUILD_DIR/$name" "$WORK/corpus" \
      -max_total_time="$GPR_FUZZ_SECONDS" -max_len=65536 \
      -timeout=10 -rss_limit_mb=1024 -artifact_prefix="$WORK/crashes/"
  done
fi
echo 'PASS: current GVID and malformed fuzz corpus'
