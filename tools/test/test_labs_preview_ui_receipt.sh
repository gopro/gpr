#!/usr/bin/env bash
# Smoke-test the Labs preview UI receipt schema.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT=${WORK:-$GPR_TMPDIR/labs_preview_ui_smoke}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/labs_preview_ui_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf "$WORK"
  fi
}
trap cleanup EXIT

"$PYTHON_BIN" - "$WORK" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
base = {
    "schema": "gpr_labs_preview_ui_receipt.v1",
    "source_provenance": {
        "available": True,
        "policy": "source_tree_digest_v1",
        "sha256": "2" * 64,
        "file_count": 4,
        "total_bytes": 256,
    },
    "target": {"name": "Pi 5 stand-in", "role": "stand-in"},
    "source": {
        "width": 4096,
        "height": 3072,
        "frame_count": 420,
        "bit_depth": 14,
        "pixel_format": 1,
        "gvid_sha256": "a" * 64,
    },
    "preview": {
        "width": 1024,
        "height": 768,
        "frame_count": 420,
        "target_fps": 20.0,
        "full_frame_downsample": True,
        "color_pipeline": "camera-wb + lightweight preview tone",
        "tone_pipeline": "fixed preview tone curve",
    },
    "integration": {
        "ui_path_executed": False,
        "decode_path": "gvid preview RGB stream",
        "presentation_path": "off-camera preview video",
        "buffer_ownership": "process-owned RGB output buffer",
        "display_surface": "stand-in file/video output",
    },
    "timing": {
        "fps_median": 36.23,
        "actual_wall_fps": 25.85,
        "median_ms": 27.6,
        "p95_ms": 34.0,
        "p99_ms": 38.0,
    },
    "memory": {"rss_kb": 65536},
    "validation": {
        "output_valid": True,
        "no_drops": True,
        "visual_checked": True,
    },
    "verdict": {
        "ui_ready": False,
        "target_evidence": True,
        "fps_target_met": True,
    },
    "blocker": {"cause": "camera UI path not executed"},
}
(root / "standin_ok.json").write_text(json.dumps(base, indent=2), encoding="utf-8")

bad = json.loads(json.dumps(base))
bad["target"] = {"name": "Mission 1", "role": "camera"}
bad["verdict"]["ui_ready"] = True
bad["integration"]["ui_path_executed"] = False
(root / "bad_promoted_ui.json").write_text(json.dumps(bad, indent=2), encoding="utf-8")

blocked_camera = json.loads(json.dumps(base))
blocked_camera["target"] = {"name": "Mission 1", "role": "camera"}
blocked_camera["integration"]["ui_path_executed"] = True
blocked_camera["integration"]["presentation_path"] = "Mission 1 camera display compositor"
blocked_camera["integration"]["buffer_ownership"] = "camera UI owns preview buffer through display present"
blocked_camera["integration"]["display_surface"] = "Mission 1 rear display"
blocked_camera["verdict"]["ui_ready"] = False
blocked_camera["validation"]["visual_checked"] = False
blocked_camera["blocker"] = {"cause": "preview visual signoff not executed on camera display"}
(root / "blocked_camera_ok.json").write_text(json.dumps(blocked_camera, indent=2), encoding="utf-8")

bad_no_cause = json.loads(json.dumps(blocked_camera))
del bad_no_cause["blocker"]
(root / "bad_blocked_without_cause.json").write_text(json.dumps(bad_no_cause, indent=2), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/check_labs_preview_ui_receipt.py" "$WORK/standin_ok.json"
"$PYTHON_BIN" "$REPO/tools/check_labs_preview_ui_receipt.py" "$WORK/blocked_camera_ok.json"

if "$PYTHON_BIN" "$REPO/tools/check_labs_preview_ui_receipt.py" "$WORK/bad_promoted_ui.json" > "$WORK/bad.log" 2>&1; then
  echo "test_labs_preview_ui_receipt: expected bad promoted UI receipt to fail" >&2
  exit 1
fi
grep -q "ui-ready receipt" "$WORK/bad.log"

if "$PYTHON_BIN" "$REPO/tools/check_labs_preview_ui_receipt.py" "$WORK/bad_blocked_without_cause.json" > "$WORK/bad_cause.log" 2>&1; then
  echo "test_labs_preview_ui_receipt: expected blocked camera receipt without cause to fail" >&2
  exit 1
fi
grep -q "blocker.cause" "$WORK/bad_cause.log"

echo "test_labs_preview_ui_receipt: PASS"
