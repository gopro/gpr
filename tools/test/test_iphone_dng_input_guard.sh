#!/usr/bin/env bash
# Local-fixture regression for iPhone DNG ingest policy.
#
# Old iPhone app DNGs can be single-plane CFA and are valid GPR inputs.
# Modern Apple ProRAW/Linear Raw files are three-sample linear raw, not Bayer;
# they must not silently roundtrip as CFA GPR.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GPR_TOOLS="${GPR_TOOLS:-$REPO/build-local/source/app/gpr_tools/gpr_tools}"

CFA_DNG="${IPHONE_CFA_DNG:-}"
LINEAR_DNG="${IPHONE_LINEAR_RAW_DNG:-}"

if [ ! -x "$GPR_TOOLS" ]; then
  echo "test_iphone_dng_input_guard: SKIP missing gpr_tools: $GPR_TOOLS"
  exit 0
fi

if [ ! -f "$CFA_DNG" ] && [ ! -f "$LINEAR_DNG" ]; then
  echo "test_iphone_dng_input_guard: SKIP missing local iPhone DNG fixtures"
  exit 0
fi

if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi

# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT="${WORK:-$GPR_EXTERNAL_ROOT/tmp/iphone_dng_input_guard}"
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/iphone_dng_input_guard.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

if [ -f "$CFA_DNG" ]; then
  "$GPR_TOOLS" -i "$CFA_DNG" -o "$WORK/iphone_cfa.gpr" -q 3 >"$WORK/cfa.stdout" 2>"$WORK/cfa.stderr"
  test -s "$WORK/iphone_cfa.gpr"
  "$GPR_TOOLS" -i "$WORK/iphone_cfa.gpr" -d 1 >"$WORK/cfa_gpr_dump.txt" 2>"$WORK/cfa_gpr_dump.stderr"
  python3 - "$WORK/cfa_gpr_dump.txt" <<'PY'
import json
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text()
start = text.find("{")
end = text.rfind("}")
if start < 0 or end < start:
    raise SystemExit("missing gpr_tools JSON dump")
payload = json.loads(text[start:end + 1])
noise_scale = payload.get("tuning_info", {}).get("noise_scale", 0)
noise_offset = payload.get("tuning_info", {}).get("noise_offset", 0)
if noise_scale <= 0 or noise_offset < 0:
    raise SystemExit(f"NoiseProfile was not preserved: scale={noise_scale} offset={noise_offset}")
PY
  "$GPR_TOOLS" -i "$WORK/iphone_cfa.gpr" -o "$WORK/iphone_cfa_roundtrip.dng" >"$WORK/cfa_roundtrip_dng.stdout" 2>"$WORK/cfa_roundtrip_dng.stderr"
  if command -v exiftool >/dev/null 2>&1; then
    exiftool -a -G1 -s -OpcodeList3 -NoiseProfile "$WORK/iphone_cfa_roundtrip.dng" >"$WORK/cfa_roundtrip_tags.txt"
    grep -q "FixVignetteRadial" "$WORK/cfa_roundtrip_tags.txt"
    grep -q "NoiseProfile" "$WORK/cfa_roundtrip_tags.txt"
  fi
  "$GPR_TOOLS" -i "$CFA_DNG" -o "$WORK/iphone_cfa_source.raw" >"$WORK/cfa_source_raw.stdout" 2>"$WORK/cfa_source_raw.stderr"
  "$GPR_TOOLS" -i "$WORK/iphone_cfa.gpr" -o "$WORK/iphone_cfa_decoded.raw" >"$WORK/cfa_decoded_raw.stdout" 2>"$WORK/cfa_decoded_raw.stderr"
  python3 - "$WORK/iphone_cfa_source.raw" "$WORK/iphone_cfa_decoded.raw" <<'PY'
import array
import math
import sys
from pathlib import Path

def load_u16(path):
    data = Path(path).read_bytes()
    out = array.array("H")
    out.frombytes(data)
    if sys.byteorder != "little":
        out.byteswap()
    return out

src = load_u16(sys.argv[1])
dec = load_u16(sys.argv[2])
if len(src) != len(dec):
    raise SystemExit(f"raw size mismatch: {len(src)} != {len(dec)}")
if max(dec) > 4095:
    raise SystemExit(f"decoded 12-bit GPR raw is not right-aligned: max={max(dec)}")
sse = 0
sad = 0
for a, b in zip(src, dec):
    d = a - b
    sad += abs(d)
    sse += d * d
mae = sad / len(src)
mse = sse / len(src)
psnr = 10 * math.log10((4095 * 4095) / mse) if mse else float("inf")
if mae > 100 or psnr < 35:
    raise SystemExit(f"raw roundtrip too far off: mae={mae:.3f} psnr12={psnr:.3f}")
PY
fi

if [ -f "$LINEAR_DNG" ]; then
  if "$GPR_TOOLS" -i "$LINEAR_DNG" -o "$WORK/iphone_linear.gpr" -q 3 >"$WORK/linear.stdout" 2>"$WORK/linear.stderr"; then
    echo "expected Apple Linear Raw DNG to be rejected" >&2
    exit 1
  fi
  grep -q "requires single-plane 2x2 RGGB/GBRG CFA input" "$WORK/linear.stderr"
  test ! -s "$WORK/iphone_linear.gpr"
fi

echo "test_iphone_dng_input_guard: PASS"
