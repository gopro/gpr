#!/usr/bin/env bash
# Smoke-test the Labs camera handoff receipt schema.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
# Caller-supplied WORK is a parent; only the unique child belongs to this test.
WORK_PARENT=${WORK:-$GPR_TMPDIR/labs_camera_handoff_smoke}
mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/labs_camera_handoff_smoke.XXXXXX")
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
    "schema": "gpr_labs_camera_handoff_receipt.v1",
    "target": {"name": "Pi 5 stand-in", "role": "stand-in"},
    "integration": {
        "raw_source_kind": "file_standin",
        "frame_source": "file-backed Bayer stand-in",
        "memory_ownership": "synchronous submit; caller owns input through return",
        "write_path": "bench_fused direct .gvid fwrite",
        "sensor_dma_handoff": {"executed": False},
        "storage_handoff": {
            "executed": False,
            "medium": "target-bench filesystem stand-in",
            "ownership": "OS/page-cache writeback; not camera firmware DMA",
        },
    },
    "input_frame": {
        "width": 8280,
        "height": 5520,
        "stride_bytes": 16560,
        "bit_depth": 14,
        "pixel_format": 4,
        "target_fps": 24.0,
    },
    "capture": {"frames_requested": 14400, "frames_written": 14400, "dropped_frames": 0},
    "timing": {"fps_median": 19.98, "median_ms": 50.04, "p95_ms": 66.01, "p99_ms": 83.0},
    "storage": {"write_mb_s": 17.46, "flush_policy": "sequential fwrite plus receipt validation"},
    "memory": {"rss_kb": 140800},
    "output": {"sha256": "0" * 64, "validation": {"valid": True, "frame_count": 14400}},
    "interruption_recovery": {"proven": True, "validator_rejects_truncated": True},
    "verdict": {
        "firmware_ready": False,
        "target_evidence": True,
        "fps_target_met": False,
        "no_drops": True,
    },
    "blocker": {"cause": "camera hardware not executed"},
}
(root / "standin_ok.json").write_text(json.dumps(base, indent=2), encoding="utf-8")

bad = json.loads(json.dumps(base))
bad["target"] = {"name": "Mission 1", "role": "camera"}
bad["verdict"]["firmware_ready"] = True
bad["integration"]["sensor_dma_handoff"]["executed"] = False
bad["integration"]["storage_handoff"]["executed"] = False
(root / "bad_promoted_camera.json").write_text(json.dumps(bad, indent=2), encoding="utf-8")

blocked_camera = json.loads(json.dumps(base))
blocked_camera["target"] = {"name": "Mission 1", "role": "camera"}
blocked_camera["integration"]["raw_source_kind"] = "sensor_dma_capture"
blocked_camera["integration"]["frame_source"] = "sensor DMA"
blocked_camera["integration"]["sensor_dma_handoff"]["executed"] = True
blocked_camera["integration"]["storage_handoff"] = {
    "executed": False,
    "medium": "Mission 1 SD path",
    "ownership": "firmware writer not yet integrated",
}
blocked_camera["blocker"] = {"cause": "storage path below target fps"}
(root / "blocked_camera_ok.json").write_text(json.dumps(blocked_camera, indent=2), encoding="utf-8")

bad_missing_storage = json.loads(json.dumps(blocked_camera))
del bad_missing_storage["integration"]["storage_handoff"]
(root / "bad_missing_storage_handoff.json").write_text(json.dumps(bad_missing_storage, indent=2), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/standin_ok.json"
"$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/blocked_camera_ok.json"

if "$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/bad_promoted_camera.json" > "$WORK/bad.log" 2>&1; then
  echo "test_labs_camera_handoff_receipt: expected bad promoted camera receipt to fail" >&2
  exit 1
fi
grep -q "firmware-ready receipt" "$WORK/bad.log"

if "$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/bad_missing_storage_handoff.json" > "$WORK/bad_storage.log" 2>&1; then
  echo "test_labs_camera_handoff_receipt: expected camera receipt without storage handoff to fail" >&2
  exit 1
fi
grep -q "storage_handoff" "$WORK/bad_storage.log"

target_bench="$WORK/labs_target_bench.json"
synthetic_gvid="$WORK/synthetic_capture.gvid"
printf 'synthetic-gvid-payload' > "$synthetic_gvid"
"$PYTHON_BIN" - "$target_bench" <<'PY'
import json
import sys
from pathlib import Path

Path(sys.argv[1]).write_text(json.dumps({
    "repo_commit": "synthetic",
    "source_provenance": {
        "available": True,
        "policy": "source_tree_digest_v1",
        "root": "/tmp/synthetic_source_snapshot",
        "sha256": "2" * 64,
        "file_count": 3,
        "total_bytes": 128,
        "included_roots": ["CMakeLists.txt", "source", "tools"],
        "git": {"available": False, "root": None, "head": None, "dirty": False, "status_short": []},
    },
    "created_utc": "2026-06-15T00:00:00Z",
    "target": {
        "name": "Pi 5 / Mission 1 stand-in",
        "fps": 24.0,
        "actual_wall_fps": 22.0,
        "actual_wall_s": 5.45,
    },
    "capture": {
        "source_width": 8280,
        "source_height": 5520,
        "pixel_format": 4,
        "frames_requested": 120,
        "frames_written": 120,
        "dropped_frames": 0,
    },
    "timing": {"n": 120, "fps_median": 25.0, "median_ms": 40.0, "p95_ms": 60.0, "p99_ms": 70.0},
    "storage": {"write_MBps_wall": 17.0, "fsync_policy": "synthetic"},
    "memory": {"bench_child_maxrss_kb": 140800},
    "gvid": {"sha256": "1" * 64, "validation": {"valid": True, "frame_count": 120}},
    "interruption_recovery": {"validator_rejects_truncated": True, "complete_frames_recovered": 119},
    "verdict": {
        "fps_target_met": False,
        "fps_median_target_met": True,
        "fps_wall_target_met": False,
        "no_drops": True,
        "gvid_valid": True,
        "interruption_recovery_proven": True,
        "target_evidence": True,
    },
}, indent=2), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/labs_target_to_camera_handoff_receipt.py" "$target_bench" \
  --output "$WORK/converted_standin.json" \
  --target-name "Pi 5 stand-in" \
  --target-role stand-in \
  --target-fps 24 \
  --blocker-cause "synthetic target below 24 fps"
"$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/converted_standin.json"
"$PYTHON_BIN" - "$WORK/converted_standin.json" <<'PY'
import json
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert receipt["timing"]["fps_median"] == 25.0
assert receipt["timing"]["actual_wall_fps"] == 22.0
assert receipt["verdict"]["fps_median_target_met"] is True
assert receipt["verdict"]["fps_wall_target_met"] is False
assert receipt["verdict"]["fps_target_met"] is False
assert receipt["integration"]["storage_handoff"]["executed"] is False
assert "stand-in" in receipt["integration"]["storage_handoff"]["medium"]
assert receipt["source_provenance"]["available"] is True
assert receipt["source_provenance"]["sha256"] == "2" * 64
PY

if "$PYTHON_BIN" "$REPO/tools/labs_target_to_camera_handoff_receipt.py" "$target_bench" \
  --output "$WORK/bad_converted_camera.json" \
  --target-name "Mission 1 camera" \
  --target-role camera \
  --target-fps 24 \
  --sensor-dma-executed \
  --storage-handoff-executed \
  > "$WORK/bad_converted_camera.log" 2>&1; then
  echo "test_labs_camera_handoff_receipt: expected camera conversion with stand-in labels to fail" >&2
  exit 1
fi
grep -q "raw-source-kind" "$WORK/bad_converted_camera.log"

if "$PYTHON_BIN" "$REPO/tools/labs_target_to_camera_handoff_receipt.py" "$target_bench" \
  --output "$WORK/bad_converted_camera_standin_labels.json" \
  --target-name "Mission 1 camera" \
  --target-role camera \
  --raw-source-kind sensor_dma_capture \
  --target-fps 24 \
  --sensor-dma-executed \
  --storage-handoff-executed \
  > "$WORK/bad_converted_camera_standin_labels.log" 2>&1; then
  echo "test_labs_camera_handoff_receipt: expected camera conversion with stand-in labels to fail" >&2
  exit 1
fi
grep -q "stand-in label" "$WORK/bad_converted_camera_standin_labels.log"

"$PYTHON_BIN" "$REPO/tools/labs_target_to_camera_handoff_receipt.py" "$target_bench" \
  --output "$WORK/converted_camera_blocked.json" \
  --target-name "Mission 1 camera" \
  --target-role camera \
  --raw-source-kind sensor_dma_capture \
  --target-fps 24 \
  --frame-source "Mission 1 sensor DMA Bayer frame callback" \
  --memory-ownership "firmware owns input until encoder return" \
  --write-path "Mission 1 firmware SD writer" \
  --sensor-dma-executed \
  --storage-handoff-executed \
  --storage-medium "Mission 1 SD card writer" \
  --storage-ownership "firmware storage queue" \
  --blocker-cause "synthetic target below 24 fps"
"$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/converted_camera_blocked.json"
"$PYTHON_BIN" - "$WORK/converted_camera_blocked.json" <<'PY'
import json
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
assert receipt["target"]["role"] == "camera"
assert receipt["integration"]["raw_source_kind"] == "sensor_dma_capture"
assert receipt["integration"]["sensor_dma_handoff"]["executed"] is True
assert receipt["integration"]["storage_handoff"]["executed"] is True
assert receipt["verdict"]["firmware_ready"] is False
assert receipt["verdict"]["fps_wall_target_met"] is False
assert receipt["blocker"]["cause"] == "synthetic target below 24 fps"
PY

"$PYTHON_BIN" - "$target_bench" "$synthetic_gvid" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
gvid_path = Path(sys.argv[2])
data = json.loads(path.read_text(encoding="utf-8"))
data["gvid"]["sha256"] = None
data["gvid"]["path"] = str(gvid_path)
data["gvid"]["sha256_policy"] = "skipped_by_test"
path.write_text(json.dumps(data, indent=2), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/labs_target_to_camera_handoff_receipt.py" "$target_bench" \
  --output "$WORK/converted_standin_computed_hash.json" \
  --target-name "Pi 5 stand-in" \
  --target-role stand-in \
  --target-fps 24 \
  --blocker-cause "synthetic target below 24 fps"
"$PYTHON_BIN" "$REPO/tools/check_labs_camera_handoff_receipt.py" "$WORK/converted_standin_computed_hash.json"
"$PYTHON_BIN" - "$WORK/converted_standin_computed_hash.json" "$synthetic_gvid" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
expected = hashlib.sha256(Path(sys.argv[2]).read_bytes()).hexdigest()
assert receipt["output"]["sha256"] == expected
PY

echo "test_labs_camera_handoff_receipt: PASS"
