#!/usr/bin/env bash
# Exercise bench_fused with live-source stand-ins. These smokes prove the
# production direct .gvid encoder can consume a FIFO stream or mmap ring source
# without falling back to preloaded file-backed frames.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${BUILD_DIR:-}" ]; then
  if [ -d "$REPO/build" ]; then
    BUILD_DIR="$REPO/build"
  else
    BUILD_DIR="$REPO/build-local"
  fi
fi
PYTHON_BIN="${PYTHON_BIN:-python3}"
GPR_TMPDIR="${GPR_TMPDIR:-${TMPDIR:-/tmp}}"
mkdir -p -- "$GPR_TMPDIR"
WORK=$(mktemp -d "$GPR_TMPDIR/bench_fused_stream_source_smoke.XXXXXX")
BENCH="$BUILD_DIR/source/app/bench_fused/bench_fused"
W=64
H=48
FRAMES=4
FRAME_BYTES=$((W * H * 2))

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf -- "$WORK"
  fi
}
trap cleanup EXIT

if [ ! -d "$BUILD_DIR" ]; then
  echo "ERROR: build directory does not exist: $BUILD_DIR" >&2
  echo "Set BUILD_DIR or run CMake configure first." >&2
  exit 1
fi
cmake --build "$BUILD_DIR" --target bench_fused -j2

make_frames_py='
import sys
from pathlib import Path
w, h, frames = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
out = Path(sys.argv[4])
with out.open("wb", buffering=0) as f:
    for idx in range(frames):
        buf = bytearray()
        for y in range(h):
            for x in range(w):
                v = (1024 + idx * 71 + x * 31 + y * 17 + x * y) & 0x3fff
                buf.extend(int(v).to_bytes(2, "little"))
        f.write(buf)
'

mkfifo "$WORK/source.fifo"
"$PYTHON_BIN" -c "$make_frames_py" "$W" "$H" "$FRAMES" "$WORK/source.fifo" &
producer_pid=$!
GPR_BENCH_STREAM_INPUT=1 \
GPR_BENCH_GVID="$WORK/fifo.gvid" \
GPR_BENCH_GVID_FPS=20 \
GPR_BENCH_PIXEL_FORMAT=4 \
FUSED_QUALITY=3 \
"$BENCH" "$WORK/source.fifo" "$W" "$H" "$FRAMES" \
  > "$WORK/fifo_stdout.txt" \
  2> "$WORK/fifo_stderr.txt"
wait "$producer_pid"

"$PYTHON_BIN" - "$REPO" "$WORK/fifo.gvid" "$WORK/fifo_stdout.txt" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
from run_labs_target_bench import validate_gvid
info = validate_gvid(Path(sys.argv[2]))
assert info["valid"] is True
assert info["frame_count"] == 4
assert info["width"] == 64
assert info["height"] == 48
text = Path(sys.argv[3]).read_text()
assert text.count("# stream_frame") == 4
assert len([line for line in text.splitlines() if line and not line.startswith("#")]) == 4
PY

"$PYTHON_BIN" - "$WORK/ring.mmap" "$FRAME_BYTES" "$FRAMES" <<'PY' &
import mmap
import struct
import sys
import time
from pathlib import Path
ring = Path(sys.argv[1])
frame_bytes = int(sys.argv[2])
frames = int(sys.argv[3])
slots = 3
stride = 64 + frame_bytes
ring.write_bytes(b"\0" * (stride * slots))
with ring.open("r+b") as fp:
    mm = mmap.mmap(fp.fileno(), stride * slots)
    try:
        for idx in range(frames):
            slot = idx % slots
            off = slot * stride
            if idx >= slots:
                want = idx - slots + 1
                while struct.unpack("<Q", mm[off + 16:off + 24])[0] < want:
                    time.sleep(0.0001)
            frame = bytes(((idx + j) & 0xff) for j in range(frame_bytes))
            mm[off + 64:off + 64 + frame_bytes] = frame
            mm[off + 8:off + 16] = struct.pack("<Q", frame_bytes)
            mm[off:off + 8] = struct.pack("<Q", idx + 1)
    finally:
        mm.close()
PY
producer_pid=$!
while [ ! -f "$WORK/ring.mmap" ]; do sleep 0.01; done
GPR_BENCH_MMAP_RING_INPUT=1 \
GPR_BENCH_MMAP_RING_SLOTS=3 \
GPR_BENCH_GVID="$WORK/mmap.gvid" \
GPR_BENCH_GVID_FPS=20 \
GPR_BENCH_PIXEL_FORMAT=4 \
FUSED_QUALITY=3 \
"$BENCH" "$WORK/ring.mmap" "$W" "$H" "$FRAMES" \
  > "$WORK/mmap_stdout.txt" \
  2> "$WORK/mmap_stderr.txt"
wait "$producer_pid"

"$PYTHON_BIN" - "$REPO" "$WORK/mmap.gvid" "$WORK/mmap_stdout.txt" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "tools"))
from run_labs_target_bench import validate_gvid
info = validate_gvid(Path(sys.argv[2]))
assert info["valid"] is True
assert info["frame_count"] == 4
assert info["width"] == 64
assert info["height"] == 48
text = Path(sys.argv[3]).read_text()
assert text.count("# stream_frame") == 4
assert len([line for line in text.splitlines() if line and not line.startswith("#")]) == 4
PY

"$PYTHON_BIN" "$REPO/tools/mission1_stream_source_encoder.py" \
  --bench "$BENCH" \
  --encoder-kind bench-fused \
  --output "$WORK/receipt_tool_bench_fused.json" \
  --work-dir "$WORK/receipt_tool_run" \
  --source-mode mmap-ring \
  --ring-slots 3 \
  --source-width "$W" \
  --source-height "$H" \
  --frames "$FRAMES" \
  --target-fps 20 \
  --quality 3 \
  --pixel-format 4 \
  --bit-depth 16

"$PYTHON_BIN" - "$WORK/receipt_tool_bench_fused.json" <<'PY'
import json
import sys
from pathlib import Path
receipt = json.loads(Path(sys.argv[1]).read_text())
assert receipt["schema"] == "gpr.mission1_stream_source_encoder.v1"
assert receipt["encoder"]["kind"] == "bench-fused"
assert receipt["source"]["mode"] == "mmap-ring"
assert receipt["encoder"]["stream_frames"] == 4
assert receipt["encoder"]["encode_write_ms"]["n"] == 4
assert receipt["output"]["validation"]["valid"] is True
assert receipt["verdict"]["stream_encode_ready"] is True
PY

echo "test_bench_fused_stream_source: PASS"
