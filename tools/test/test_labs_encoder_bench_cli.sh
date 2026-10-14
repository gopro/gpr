#!/usr/bin/env bash
# Exercise the firmware-facing Labs encoder shim through the target-bench
# receipt wrapper. This proves the public shim can produce a valid .gvid
# stream through the same receipt path used by Mission 1 closure runs.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR="${BUILD_DIR:-$REPO/build-local}"
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
mkdir -p -- "$GPR_TMPDIR"
WORK=$(mktemp -d "$GPR_TMPDIR/labs_encoder_bench_cli_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"
SMOKE_W="${GPR_LABS_BENCH_SMOKE_W:-512}"
SMOKE_H="${GPR_LABS_BENCH_SMOKE_H:-384}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf -- "$WORK"
  fi
}
trap cleanup EXIT

cmake --build "$BUILD_DIR" --target labs_encoder_bench_cli -j2

"$PYTHON_BIN" - "$WORK/frame.raw" "$SMOKE_W" "$SMOKE_H" <<'PY'
import sys
from pathlib import Path
W, H = int(sys.argv[2]), int(sys.argv[3])
buf = bytearray()
for y in range(H):
    for x in range(W):
        v = (1024 + x * 31 + y * 17 + x * y) & 0x3fff
        buf.extend(int(v).to_bytes(2, "little"))
Path(sys.argv[1]).write_bytes(buf)
PY

GPR_BENCH_GVID="$WORK/direct.gvid" \
GPR_BENCH_GVID_FPS=20 \
GPR_BENCH_PIXEL_FORMAT=4 \
FUSED_QUALITY=3 \
"$BUILD_DIR/bin/labs_encoder_bench_cli" "$WORK/frame.raw" "$SMOKE_W" "$SMOKE_H" 4 \
  > "$WORK/bench_stdout.txt" \
  2> "$WORK/bench_stderr.txt"

"$PYTHON_BIN" "$REPO/tools/run_labs_target_bench.py" \
  --bench "$BUILD_DIR/bin/labs_encoder_bench_cli" \
  --raw "$WORK/frame.raw" \
  --frames 4 \
  --output-dir "$WORK/receipt" \
  --source-width "$SMOKE_W" \
  --source-height "$SMOKE_H" \
  --capture-width "$SMOKE_W" \
  --capture-height "$SMOKE_H" \
  --target-fps 20 \
  --quality 3 \
  --pixel-format 4 \
  --direct-gvid \
  --source-provenance-root "$REPO"

"$PYTHON_BIN" - "$WORK/receipt/labs_target_bench.json" "$SMOKE_W" "$SMOKE_H" <<'PY'
import json
import sys
from pathlib import Path
receipt = json.loads(Path(sys.argv[1]).read_text())
W, H = int(sys.argv[2]), int(sys.argv[3])
assert receipt["schema"] == "gpr_labs_target_bench.v1"
assert receipt["capture"]["frames_requested"] == 4
assert receipt["capture"]["frames_written"] == 4
assert receipt["capture"]["capture_width"] == W
assert receipt["capture"]["capture_height"] == H
assert receipt["gvid"]["validation"]["valid"] is True
assert receipt["gvid"]["validation"]["frame_count"] == 4
assert receipt["gvid"]["validation"]["fps_x1000"] == 20000
assert receipt["bench"]["build"]["binary"].endswith("labs_encoder_bench_cli")
assert receipt["bench_phase_timing"]["available"] is True
assert receipt["writer_handoff"]["wall_includes_writer_drain"] is True
assert receipt["verdict"]["gvid_valid"] is True
assert receipt["verdict"]["storage_target_met"] is True
PY

"$PYTHON_BIN" "$REPO/tools/mission1_stream_source_encoder.py" \
  --bench "$BUILD_DIR/bin/labs_encoder_bench_cli" \
  --output "$WORK/stream_receipt.json" \
  --work-dir "$WORK/stream" \
  --source-width "$SMOKE_W" \
  --source-height "$SMOKE_H" \
  --frames 4 \
  --target-fps 20 \
  --quality 3 \
  --pixel-format 4 \
  --bit-depth 16 \
  --delay-pattern-ms 0,0.5

"$PYTHON_BIN" - "$WORK/stream_receipt.json" "$SMOKE_W" "$SMOKE_H" <<'PY'
import json
import sys
from pathlib import Path
receipt = json.loads(Path(sys.argv[1]).read_text())
W, H = int(sys.argv[2]), int(sys.argv[3])
assert receipt["schema"] == "gpr.mission1_stream_source_encoder.v1"
assert receipt["target"]["role"] == "stand-in"
assert receipt["target"]["not_camera_evidence"] is True
assert receipt["source"]["frame_bytes"] == W * H * 2
assert receipt["producer"]["process"] == "separate"
assert receipt["encoder"]["process"] == "separate"
assert receipt["producer"]["frames_written"] == 4
assert receipt["encoder"]["stream_frames"] == 4
assert receipt["encoder"]["labs_encoder_stats"]["written"] == 4
assert receipt["output"]["validation"]["valid"] is True
assert receipt["output"]["validation"]["frame_count"] == 4
assert receipt["verdict"]["stream_encode_ready"] is True
assert receipt["verdict"]["production_evidence"] is False
PY

"$PYTHON_BIN" "$REPO/tools/mission1_stream_source_encoder.py" \
  --bench "$BUILD_DIR/bin/labs_encoder_bench_cli" \
  --output "$WORK/mmap_ring_receipt.json" \
  --work-dir "$WORK/mmap_ring" \
  --source-mode mmap-ring \
  --ring-slots 3 \
  --source-width "$SMOKE_W" \
  --source-height "$SMOKE_H" \
  --frames 4 \
  --target-fps 20 \
  --quality 3 \
  --pixel-format 4 \
  --bit-depth 16 \
  --delay-pattern-ms 0,0.5

"$PYTHON_BIN" - "$WORK/mmap_ring_receipt.json" <<'PY'
import json
import sys
from pathlib import Path
receipt = json.loads(Path(sys.argv[1]).read_text())
assert receipt["schema"] == "gpr.mission1_stream_source_encoder.v1"
assert receipt["source"]["mode"] == "mmap-ring"
assert receipt["source"]["ring_slots"] == 3
assert receipt["producer"]["process"] == "separate"
assert receipt["encoder"]["process"] == "separate"
assert receipt["producer"]["frames_written"] == 4
assert receipt["encoder"]["stream_frames"] == 4
assert receipt["encoder"]["labs_encoder_stats"]["written"] == 4
assert receipt["output"]["validation"]["valid"] is True
assert receipt["verdict"]["stream_encode_ready"] is True
assert receipt["verdict"]["production_evidence"] is False
PY

echo "test_labs_encoder_bench_cli: PASS"
