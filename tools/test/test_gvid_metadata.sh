#!/usr/bin/env bash
# Smoke-test .gvid source metadata validation against a packed two-frame stream.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
WORK_PARENT=${WORK:-$GPR_TMPDIR}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/gvid_metadata_smoke.XXXXXX")
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
printf 'frame-0001-payload' > "$WORK/frames/frame_0001.gpr"

"$PYTHON_BIN" "$REPO/tools/gvid_pack.py" "$WORK/frames" "$WORK/clip.gvid" \
  --width 8280 --height 5520 --fps 24 --quality 3 --pixel-format 4

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
          "accepted": false,
          "reject_reasons": ["lag"],
          "sigma_rms_counts": 12.5,
          "exact_residual_to_sigma_rms": 0.0,
          "lag_max_abs": 0.3,
          "edge_removed_energy_ratio": 1.2
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
          "accepted": true,
          "reject_reasons": [],
          "sigma_rms_counts": 110.0,
          "exact_residual_to_sigma_rms": 0.24,
          "lag_max_abs": 0.1,
          "edge_removed_energy_ratio": 0.8
        }
      ]
    }
  ]
}
JSON

"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" validate "$WORK/clip.gvid.meta.json" \
  --gvid "$WORK/clip.gvid"

"$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" runtime-dispatch "$WORK/clip.gvid.meta.json" \
  --gvid "$WORK/clip.gvid" \
  --output "$WORK/clip.gvid.dispatch.json"
"$PYTHON_BIN" - "$WORK/clip.gvid.dispatch.json" <<'PY'
import json
import sys
from pathlib import Path

dispatch = json.loads(Path(sys.argv[1]).read_text())
assert dispatch["schema"] == "gvid_runtime_dispatch.v1"
assert dispatch["frame_count"] == 2
assert dispatch["tile_count"] == 2
assert dispatch["accepted_tile_count"] == 1
assert dispatch["frames"][0]["frame_tag"] == 0
assert dispatch["frames"][0]["payload_size"] > 0
assert dispatch["frames"][0]["raw_clean_tiles"][0]["policy"] == "all_targets_raw_clean"
assert dispatch["frames"][1]["raw_clean_tiles"][0]["policy"] == "accepted_only_raw_clean"
PY

"$PYTHON_BIN" - "$WORK/clip.gvid" "$WORK/dup_tag.gvid" <<'PY'
import struct
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
data = bytearray(src.read_bytes())
pos = 32
magic, size, tag = struct.unpack("<IIQ", data[pos:pos + 16])
assert tag == 0
pos += 16 + size
magic, size, tag = struct.unpack("<IIQ", data[pos:pos + 16])
assert tag == 1
data[pos + 8:pos + 16] = struct.pack("<Q", 0)
dst.write_bytes(data)
PY

if "$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" runtime-dispatch "$WORK/clip.gvid.meta.json" \
  --gvid "$WORK/dup_tag.gvid" \
  --output "$WORK/dup_tag.gvid.dispatch.json"; then
  echo "expected duplicate stream frame tags to fail runtime dispatch" >&2
  exit 1
fi

"$PYTHON_BIN" - "$WORK/clip.gvid.meta.json" "$WORK/bad_tags.gvid.meta.json" <<'PY'
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
meta = json.loads(src.read_text())
meta["frames"][0]["frame_tag"], meta["frames"][1]["frame_tag"] = 1, 0
dst.write_text(json.dumps(meta, indent=2))
PY

if "$PYTHON_BIN" "$REPO/tools/gvid_metadata.py" validate "$WORK/bad_tags.gvid.meta.json" \
  --gvid "$WORK/clip.gvid"; then
  echo "expected swapped frame tags to fail validation" >&2
  exit 1
fi

echo "test_gvid_metadata: PASS"
