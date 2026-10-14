#!/usr/bin/env bash
# Smoke-test gpr2prores direct .gvid input on macOS with local fixtures.
# WORK selects the parent for unique scratch; artifacts are retained by default.
# Set GPR_KEEP_TEST_ARTIFACTS=0 to remove owned scratch on exit.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
GPR2PRORES=${GPR2PRORES:-"$REPO/tools/gpr2prores/gpr2prores"}
PYTHON_BIN=${PYTHON_BIN:-python3}
SRC_GPR=${SRC_GPR:-}
META_DNG=${META_DNG:-}
WORK_PARENT=${WORK:-"${GPR_TMPDIR:-${TMPDIR:-/tmp}}/gpr2prores_gvid_smoke"}

if [ "$(uname -s)" != "Darwin" ]; then
  echo "test_gpr2prores_gvid_input: SKIP non-macOS"
  exit 0
fi
if [ ! -x "$GPR2PRORES" ]; then
  echo "test_gpr2prores_gvid_input: SKIP missing gpr2prores binary: $GPR2PRORES"
  exit 0
fi
if [ ! -f "$SRC_GPR" ] || [ ! -f "$META_DNG" ]; then
  echo "test_gpr2prores_gvid_input: SKIP missing local fixtures"
  echo "  SRC_GPR=$SRC_GPR"
  echo "  META_DNG=$META_DNG"
  exit 0
fi

mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/gvid-smoke.XXXXXX")
cleanup_run() {
  if [ "${GPR_KEEP_TEST_ARTIFACTS:-1}" = 1 ]; then
    printf 'Scratch retained: %s\n' "$WORK"
  else
    rm -rf -- "$WORK"
  fi
}
trap cleanup_run EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$WORK/frames" "$WORK/tmp"
cp "$SRC_GPR" "$WORK/frames/frame_0000.gpr"

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/oneframe.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4
"$PYTHON_BIN" - "$WORK/oneframe.gvid" "$WORK/oneframe.gvid.dispatch.json" <<'PY'
import json
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
magic, payload_size, frame_tag = struct.unpack("<IIQ", data[32:48])
assert magic == 0x004D5246

Path(sys.argv[2]).write_text(json.dumps({
    "schema": "gvid_runtime_dispatch.v1",
    "gvid": "oneframe.gvid",
    "metadata": None,
    "frame_count": 1,
    "tile_count": 2,
    "accepted_tile_count": 1,
    "frames": [
        {
            "frame_index": 0,
            "frame_tag": frame_tag,
            "payload_offset": 48,
            "payload_size": payload_size,
            "source_id": "Z8Z_0258",
            "source_path": "Z8Z_0258.dng",
            "iso": 64,
            "raw_clean_tiles": [
                {
                    "crop": "A",
                    "source_xywh": [0, 0, 64, 64],
                    "accepted": True,
                    "policy": "accepted_only_raw_clean",
                    "reject_reasons": [],
                    "sigma_rms_counts": 1.0,
                },
                {
                    "crop": "B",
                    "source_xywh": [64, 0, 64, 64],
                    "accepted": False,
                    "policy": "all_targets_raw_clean",
                    "reject_reasons": ["smoke"],
                    "sigma_rms_counts": 1.0,
                },
            ],
        }
    ],
}, indent=2) + "\n")
PY

TMPDIR="$WORK/tmp" "$GPR2PRORES" \
  --phase0 --no-cnn \
  --gvid-dispatch "$WORK/oneframe.gvid.dispatch.json" \
  --meta-dng "$META_DNG" \
  "$WORK/oneframe.gvid" "$WORK/out.mov" \
  2>&1 | tee "$WORK/phase0.log"

grep -q "magic=0x44535546" "$WORK/phase0.log"
grep -q "$WORK/tmp/gpr2prores_gvid_.*/frame_000000.gpr" "$WORK/phase0.log"
grep -q "accepted_only=1 all_targets=1" "$WORK/phase0.log"

"$PYTHON_BIN" - "$WORK/oneframe.gvid.dispatch.json" "$WORK/bad_policy.dispatch.json" <<'PY'
import sys
from pathlib import Path

src = Path(sys.argv[1]).read_text()
Path(sys.argv[2]).write_text(src.replace("accepted_only_raw_clean", "unknown_policy", 1))
PY
if TMPDIR="$WORK/tmp" "$GPR2PRORES" \
  --phase0 --no-cnn \
  --gvid-dispatch "$WORK/bad_policy.dispatch.json" \
  --meta-dng "$META_DNG" \
  "$WORK/oneframe.gvid" "$WORK/bad_policy.mov" \
  >"$WORK/bad_policy.log" 2>&1; then
  echo "expected invalid dispatch policy failure" >&2
  exit 1
fi
grep -q "unknown policy" "$WORK/bad_policy.log"

"$PYTHON_BIN" - "$WORK/oneframe.gvid.dispatch.json" "$WORK/bad_payload.dispatch.json" <<'PY'
import json
import sys
from pathlib import Path

plan = json.loads(Path(sys.argv[1]).read_text())
plan["frames"][0]["payload_size"] += 1
Path(sys.argv[2]).write_text(json.dumps(plan, indent=2) + "\n")
PY
if TMPDIR="$WORK/tmp" "$GPR2PRORES" \
  --phase0 --no-cnn \
  --gvid-dispatch "$WORK/bad_payload.dispatch.json" \
  --meta-dng "$META_DNG" \
  "$WORK/oneframe.gvid" "$WORK/bad_payload.mov" \
  >"$WORK/bad_payload.log" 2>&1; then
  echo "expected dispatch stream-header mismatch failure" >&2
  exit 1
fi
grep -q "does not match GVID stream header" "$WORK/bad_payload.log"

mkdir -p "$WORK/order_frames"
cp "$SRC_GPR" "$WORK/order_frames/frame_0000.gpr"
"$PYTHON_BIN" - "$WORK/order_frames/frame_0001.gpr" <<'PY'
import struct
import sys
from pathlib import Path

payload = bytearray(64)
payload[0:4] = struct.pack("<I", 0x21444142)  # "BAD!" in little endian.
payload[8:16] = struct.pack("<II", 1234, 5678)
Path(sys.argv[1]).write_bytes(payload)
PY
"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/order_frames" "$WORK/out_of_order_tags.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4
"$PYTHON_BIN" - "$WORK/out_of_order_tags.gvid" <<'PY'
import struct
import sys
from pathlib import Path

p = Path(sys.argv[1])
data = bytearray(p.read_bytes())
pos = 32
magic, size, tag = struct.unpack("<IIQ", data[pos:pos + 16])
assert tag == 0
data[pos + 8:pos + 16] = struct.pack("<Q", 7)
pos += 16 + size
magic, size, tag = struct.unpack("<IIQ", data[pos:pos + 16])
assert tag == 1
data[pos + 8:pos + 16] = struct.pack("<Q", 3)
p.write_bytes(data)
PY
TMPDIR="$WORK/tmp" "$GPR2PRORES" \
  --phase0 --no-cnn \
  --meta-dng "$META_DNG" \
  "$WORK/out_of_order_tags.gvid" "$WORK/order.mov" \
  2>&1 | tee "$WORK/order.log"
grep -q "Phase 0 (GPR): .*frame_000000.gpr" "$WORK/order.log"
grep -q "magic=0x44535546" "$WORK/order.log"

TMPDIR="$WORK/tmp" "$GPR2PRORES" \
  --max-frames 1 --no-cnn \
  --demosaic core-image --out-resolution 2k \
  --gvid-dispatch "$WORK/oneframe.gvid.dispatch.json" \
  --meta-dng "$META_DNG" \
  "$WORK/oneframe.gvid" "$WORK/out_2k.mov" \
  2>&1 | tee "$WORK/render.log"

test -s "$WORK/out_2k.mov"
grep -q "errors=0" "$WORK/render.log"

if command -v ffprobe >/dev/null 2>&1; then
  mkdir -p "$WORK/fps_frames"
  cp "$SRC_GPR" "$WORK/fps_frames/frame_0000.gpr"
  cp "$SRC_GPR" "$WORK/fps_frames/frame_0001.gpr"
  "$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/fps_frames" "$WORK/twoframe.gvid" \
    --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4
  TMPDIR="$WORK/tmp" "$GPR2PRORES" \
    --max-frames 2 --fps 24 --no-cnn \
    --demosaic core-image --out-resolution 2k \
    --meta-dng "$META_DNG" \
    "$WORK/twoframe.gvid" "$WORK/twoframe_2k.mov" \
    2>&1 | tee "$WORK/twoframe_render.log"
  test -s "$WORK/twoframe_2k.mov"
  grep -q "errors=0" "$WORK/twoframe_render.log"
  "$PYTHON_BIN" - "$WORK/twoframe_2k.mov" <<'PY'
import json
import subprocess
import sys

probe = json.loads(subprocess.check_output([
    "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", sys.argv[1]
]))
stream = probe["streams"][0]
assert stream["nb_frames"] == "2", stream
assert stream["time_base"] == "1/24", stream
assert stream["duration_ts"] == 2, stream
assert stream["r_frame_rate"] == "24/1", stream
assert stream["avg_frame_rate"] == "24/1", stream
PY
else
  echo "test_gpr2prores_gvid_input: SKIP ProRes fps metadata check; ffprobe missing"
fi

cp "$WORK/oneframe.gvid" "$WORK/dup_tag.gvid"
"$PYTHON_BIN" - "$WORK/dup_tag.gvid" <<'PY'
import struct
import sys
from pathlib import Path

p = Path(sys.argv[1])
data = bytearray(p.read_bytes())
pos = 32
magic, size, tag = struct.unpack("<IIQ", data[pos:pos + 16])
frame = data[pos:pos + 16 + size]
out = bytearray(data[:32] + frame + frame)
out[28:32] = struct.pack("<I", 2)
p.write_bytes(out)
PY

if TMPDIR="$WORK/tmp" "$GPR2PRORES" \
  --phase0 --no-cnn \
  --meta-dng "$META_DNG" \
  "$WORK/dup_tag.gvid" "$WORK/dup.mov" \
  >"$WORK/dup.log" 2>&1; then
  echo "expected duplicate frame tag failure" >&2
  exit 1
fi
grep -q "duplicate GVID frame tag" "$WORK/dup.log"

echo "test_gpr2prores_gvid_input: PASS"
