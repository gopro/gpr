#!/usr/bin/env bash
# Smoke-test that gpr_tools RAW -> GPR produces an editable/readable GPR by
# default, including when the retired experimental switch is present.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GPR_TOOLS=${GPR_TOOLS:-"${BUILD_DIR:-$REPO/build}/source/app/gpr_tools/gpr_tools"}
PYTHON_BIN=${PYTHON_BIN:-python3}
WORK_PARENT=${WORK:-"${GPR_TMPDIR:-${TMPDIR:-/tmp}}"}

if [ ! -x "$GPR_TOOLS" ]; then
  echo "test_gpr_tools_raw_gpr_roundtrip: ERROR missing gpr_tools: $GPR_TOOLS" >&2
  exit 2
fi
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/gpr_tools_raw_gpr_roundtrip.XXXXXX")

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

"$PYTHON_BIN" - "$WORK/in.raw" <<'PY'
import struct
import sys

w, h = 512, 384
with open(sys.argv[1], "wb") as f:
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += struct.pack("<H", (x * 7 + y * 11) & 0x3fff)
        f.write(row)
PY

"$GPR_TOOLS" -i "$WORK/in.raw" -w 512 -h 384 -x rggb14 \
  -o "$WORK/sdk_wrapped.gpr" -q 3 >"$WORK/raw_to_gpr.log" 2>&1
"$GPR_TOOLS" -i "$WORK/sdk_wrapped.gpr" \
  -o "$WORK/sdk_wrapped.raw" >"$WORK/gpr_to_raw.log" 2>&1

GPR_FAST_RAW_TO_GPR=1 "$GPR_TOOLS" -i "$WORK/in.raw" -w 512 -h 384 -x rggb14 \
  -o "$WORK/retired_switch.gpr" -q 3 >"$WORK/retired_switch.log" 2>&1
grep -q 'retired; using the metadata-preserving SDK writer' "$WORK/retired_switch.log"
"$GPR_TOOLS" -i "$WORK/retired_switch.gpr" \
  -o "$WORK/retired_switch.raw" >"$WORK/retired_decode.log" 2>&1
cmp "$WORK/sdk_wrapped.raw" "$WORK/retired_switch.raw"

test -s "$WORK/sdk_wrapped.gpr"
test -s "$WORK/sdk_wrapped.raw"

"$PYTHON_BIN" - "$WORK/in.raw" "$WORK/sdk_wrapped.raw" <<'PY'
import math
import struct
import sys

src_b = open(sys.argv[1], "rb").read()
dec_b = open(sys.argv[2], "rb").read()
if len(src_b) != len(dec_b):
    raise SystemExit(f"size mismatch: {len(src_b)} != {len(dec_b)}")
count = len(src_b) // 2
err = 0.0
for i in range(count):
    src = struct.unpack_from("<H", src_b, i * 2)[0]
    dec = struct.unpack_from("<H", dec_b, i * 2)[0]
    d = float(src - dec)
    err += d * d
mse = err / float(count)
psnr = 999.0 if mse == 0 else 10.0 * math.log10((16383.0 ** 2) / mse)
if psnr < 40.0:
    raise SystemExit(f"PSNR too low: {psnr:.2f} dB")
print(f"raw_gpr_roundtrip_psnr14={psnr:.2f}dB")
PY

echo "test_gpr_tools_raw_gpr_roundtrip: PASS"
