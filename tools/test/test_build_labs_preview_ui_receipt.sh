#!/usr/bin/env bash
# Smoke-test the Labs preview UI receipt builder.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT=${WORK:-$GPR_TMPDIR/labs_preview_ui_builder_smoke}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/labs_preview_ui_builder_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

mkdir -p "$WORK/preview"

"$PYTHON_BIN" - "$WORK" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
(root / "target.json").write_text(json.dumps({
    "source_provenance": {
        "available": True,
        "policy": "source_tree_digest_v1",
        "sha256": "1" * 64,
        "file_count": 12,
        "total_bytes": 3456,
    },
    "target": {"fps": 20.0},
    "capture": {
        "capture_width": 4096,
        "capture_height": 3072,
        "pixel_format": 1,
        "frames_written": 1440,
        "dropped_frames": 0,
    },
}), encoding="utf-8")
(root / "preview" / "receipt.json").write_text(json.dumps({
    "gvid_sha256": "a" * 64,
    "frame_count": 1440,
    "summary": {
        "dims": [[1024, 768]],
        "actual_wall_fps_including_extract_process": 23.5,
        "decode_plus_target": {
            "fps_median": 42.5,
            "median_ms": 23.5,
            "p95_ms": 27.6,
            "p99_ms": 28.8,
        },
        "process_wall": {"median_ms": 28.8},
    },
    "memory": {"children_maxrss_kb": 70000},
}), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/build_labs_preview_ui_receipt.py" \
  --target-bench "$WORK/target.json" \
  --preview-receipt "$WORK/preview/receipt.json" \
  --output "$WORK/preview_ui_receipt.json"
"$PYTHON_BIN" "$REPO/tools/check_labs_preview_ui_receipt.py" "$WORK/preview_ui_receipt.json"

"$PYTHON_BIN" - "$WORK/preview_ui_receipt.json" <<'PY'
import json
import sys
from pathlib import Path

data = json.loads(Path(sys.argv[1]).read_text())
assert data["verdict"]["ui_ready"] is False
assert data["verdict"]["fps_target_met"] is True
assert data["source"]["frame_count"] == 1440
assert data["preview"]["width"] == 1024
assert data["blocker"]["cause"] == "camera UI path not executed"
PY

if "$PYTHON_BIN" "$REPO/tools/build_labs_preview_ui_receipt.py" \
  --target-bench "$WORK/target.json" \
  --preview-receipt "$WORK/preview/receipt.json" \
  --output "$WORK/bad_camera_preview_ui_receipt.json" \
  --target-role camera \
  --ui-path-executed \
  --visual-checked \
  > "$WORK/bad_camera.log" 2>&1; then
  echo "test_build_labs_preview_ui_receipt: expected camera receipt with stand-in labels to fail" >&2
  exit 1
fi
grep -q "stand-in label" "$WORK/bad_camera.log"

"$PYTHON_BIN" "$REPO/tools/build_labs_preview_ui_receipt.py" \
  --target-bench "$WORK/target.json" \
  --preview-receipt "$WORK/preview/receipt.json" \
  --output "$WORK/camera_preview_ui_receipt.json" \
  --target-name "Mission 1 camera" \
  --target-role camera \
  --ui-path-executed \
  --visual-checked \
  --display-surface "Mission 1 rear display scanout" \
  --presentation-path "firmware preview compositor" \
  --buffer-ownership "firmware-owned RGB scanout buffer" \
  --decode-path "Mission 1 fused decode preview path" \
  --blocker-cause "none"
"$PYTHON_BIN" "$REPO/tools/check_labs_preview_ui_receipt.py" "$WORK/camera_preview_ui_receipt.json"
"$PYTHON_BIN" - "$WORK/camera_preview_ui_receipt.json" <<'PY'
import json
import sys
from pathlib import Path

data = json.loads(Path(sys.argv[1]).read_text())
assert data["target"]["role"] == "camera"
assert data["integration"]["ui_path_executed"] is True
assert data["verdict"]["ui_ready"] is True
assert data["blocker"]["cause"] == "none"
PY

echo "test_build_labs_preview_ui_receipt: PASS"
