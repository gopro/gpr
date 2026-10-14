#!/usr/bin/env bash
# Smoke-test the neutral .gvid packer against the documented wire header.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
WORK_PARENT=${WORK:-$GPR_TMPDIR}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/gvid_pack_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

mkdir -p "$WORK/frames"
printf 'frame-0000-payload' > "$WORK/frames/frame_0000.gpr"
printf 'frame-0001-payload-longer' > "$WORK/frames/frame_0001.gpr"
mkdir -p "$WORK/native_frames"
printf 'native-camera-gpr-0000' > "$WORK/native_frames/GP017601.GPR"
printf 'native-camera-gpr-0001-longer' > "$WORK/native_frames/GP017602.GPR"

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip_q11.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 11 --pixel-format 4

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip_pf5.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 5

"$PYTHON_BIN" - "$WORK/clip.gvid" <<'PY'
import struct
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
clip = struct.unpack("<IBBHHHIIIII", data[:32])
assert clip[0] == 0x44495647, clip
assert clip[1] == 1, clip
assert clip[3] == 4, clip
assert clip[4] == 3, clip
assert clip[6] == 8280 and clip[7] == 5520, clip
assert clip[8] == 24000, clip
assert clip[10] == 2, clip
pos = 32
for idx, expected in enumerate([b"frame-0000-payload", b"frame-0001-payload-longer"]):
    magic, size, tag = struct.unpack("<IIQ", data[pos:pos + 16])
    assert magic == 0x004D5246, (magic, idx)
    assert size == len(expected), (size, len(expected), idx)
    assert tag == idx, (tag, idx)
    pos += 16
    assert data[pos:pos + size] == expected, idx
    pos += size
assert pos == len(data), (pos, len(data))
PY

"$PYTHON_BIN" - "$WORK/clip_q11.gvid" <<'PY'
import struct
import sys
from pathlib import Path

clip = struct.unpack("<IBBHHHIIIII", Path(sys.argv[1]).read_bytes()[:32])
assert clip[4] == 11, clip
PY

if "$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip_q12.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 12 --pixel-format 4; then
  echo "expected q12 pack to fail" >&2
  exit 1
fi
test ! -e "$WORK/clip_q12.gvid"

if "$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip_pf6.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 6; then
  echo "expected pixel-format 6 pack to fail" >&2
  exit 1
fi
test ! -e "$WORK/clip_pf6.gvid"

cat > "$WORK/clip.gvid.meta.json" <<'JSON'
{
  "schema": "gvid_source_metadata.v1",
  "source": "smoke-test",
  "targets": null,
  "gvid": "clip.gvid",
  "frame_count": 2,
  "frames": [
    {
      "frame_index": 0,
      "frame_tag": 0,
      "source_id": "frame_0000",
      "source_path": "frame_0000.dng",
      "iso": 64,
      "raw_clean_tiles": [
        {
          "crop": "A_detail",
          "source_xywh": [3000, 2000, 512, 512],
          "accepted": true,
          "reject_reasons": [],
          "sigma_rms_counts": 12.5,
          "exact_residual_to_sigma_rms": 0.2,
          "lag_max_abs": 0.1,
          "edge_removed_energy_ratio": 0.8
        }
      ]
    },
    {
      "frame_index": 1,
      "frame_tag": 1,
      "source_id": "frame_0001",
      "source_path": "frame_0001.dng",
      "iso": 5000,
      "raw_clean_tiles": [
        {
          "crop": "A_detail",
          "source_xywh": [3000, 2000, 512, 512],
          "accepted": false,
          "reject_reasons": ["lag"],
          "sigma_rms_counts": 110.0,
          "exact_residual_to_sigma_rms": 0.0,
          "lag_max_abs": 0.3,
          "edge_removed_energy_ratio": 1.2
        }
      ]
    }
  ]
}
JSON

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip_with_meta.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4 \
  --metadata "$WORK/clip.gvid.meta.json"

test -f "$WORK/clip_with_meta.gvid.meta.json"
"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" validate "$WORK/clip_with_meta.gvid.meta.json" \
  --gvid "$WORK/clip_with_meta.gvid"
"$PYTHON_BIN" - "$WORK/clip_with_meta.gvid.meta.json" <<'PY'
import json
import sys
from pathlib import Path

meta = json.loads(Path(sys.argv[1]).read_text())
assert meta["gvid"] == "clip_with_meta.gvid", meta["gvid"]
PY

"$PYTHON_BIN" - "$WORK/clip.gvid.meta.json" "$WORK/bad_attach.gvid.meta.json" <<'PY'
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
meta = json.loads(src.read_text())
meta["frames"][0]["frame_tag"], meta["frames"][1]["frame_tag"] = 1, 0
dst.write_text(json.dumps(meta, indent=2))
PY

if "$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/bad_attach.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4 \
  --metadata "$WORK/bad_attach.gvid.meta.json"; then
  echo "expected bad metadata attach to fail" >&2
  exit 1
fi
test ! -e "$WORK/bad_attach.gvid"

"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" from-native-gpr-sequence "$WORK/native_frames" \
  "$WORK/native.gvid.meta.json" \
  --width 4096 --height 3072 --fps 24

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/native_frames" "$WORK/native.gvid" \
  --width 4096 --height 3072 --fps 24 --quality 0 --pixel-format 1 \
  --payload-kind camera_gpr \
  --metadata "$WORK/native.gvid.meta.json"

"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" validate "$WORK/native.gvid.meta.json" \
  --gvid "$WORK/native.gvid"
"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" runtime-dispatch "$WORK/native.gvid.meta.json" \
  --gvid "$WORK/native.gvid" \
  --output "$WORK/native.gvid.dispatch.json"
"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" verify-payloads "$WORK/native.gvid.meta.json" \
  --gvid "$WORK/native.gvid" \
  --output "$WORK/native.gvid.payload_verify.json"
"$PYTHON_BIN" - "$WORK/native.gvid.dispatch.json" <<'PY'
import json
import sys
from pathlib import Path

dispatch = json.loads(Path(sys.argv[1]).read_text())
assert dispatch["payload_kind"] == "camera_gpr"
assert dispatch["frame_count"] == 2
assert dispatch["tile_count"] == 0
assert dispatch["frames"][0]["source_id"] == "GP017601"
assert dispatch["frames"][0]["payload_sha256"]
PY
"$PYTHON_BIN" - "$WORK/native.gvid.payload_verify.json" <<'PY'
import json
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text())
assert receipt["schema"] == "gvid_camera_gpr_payload_verify.v1"
assert receipt["frame_count"] == 2
assert receipt["all_exact"] is True
assert all(frame["ok"] for frame in receipt["frames"])
PY

echo "test_gvid_pack: PASS"
