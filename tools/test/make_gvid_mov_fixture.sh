#!/usr/bin/env bash
# make_gvid_mov_fixture.sh — turn a directory of DNGs (or NEFs via DNG Converter)
# into a 24-frame .mov container playable by gpr2prores.
#
# This is the recipe used to build the BarnAndSky daytime fixture used by
# the sustained-throughput test. It exists in the repo so future fixtures
# (other timelapses, other sensors) can be made the same way without
# rediscovering the steps.
#
# Inputs:
#   $1: source directory (DNGs or NEFs)
#   $2: output .mov path
# Optional env:
#   FRAMES=24   (number of frames to take)
#   START=mid   (mid|0|N — where to start in the source dir)
#   FPS=24      (container framerate metadata)
#   QUALITY=3   (GPR quality preset 0-11)
#   BUILD_DIR=build (cmake build root; GTOOLS/BENCH/MOV override tool paths)
#   WORK=...    (parent for a unique scratch directory)
#   GPR_KEEP_TEST_ARTIFACTS=1 (retain scratch; otherwise removed on exit)
#
# Outputs:
#   $2 — the .mov container
#   Scratch gpr_files/ — per-frame FUSED .gpr (intermediate)
#   Scratch dng_files/ — per-frame DNG (intermediate, if NEFs were converted)
#
# Process:
#   1. If source files are .NEF, run Adobe DNG Converter to make .dng copies
#      (DNG Converter handles new sensor firmware libraw doesn't yet support).
#   2. Extract raw bayer plane from each DNG via rawpy.
#   3. Run bench_fused with GPR_INCLUDE_LL=1 to emit FUSED .gpr per frame.
#      LL inclusion is required — without it the decoder rejects with rc=-5
#      ("unsupported: no preserved lowpass").
#   4. Pack the per-frame .gpr into a .mov container via gpr_mov_tool pack.
#
# The fixture validates the pipeline end-to-end and provides a payload
# for regression tests and performance measurements.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <source_dir> <output.mov>" >&2
    exit 2
fi
SRC="$1"
OUT="$2"
FRAMES="${FRAMES:-24}"
START="${START:-mid}"
FPS="${FPS:-24}"
QUALITY="${QUALITY:-3}"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$REPO/$BUILD_DIR"
GTOOLS="${GTOOLS:-$BUILD_DIR/source/app/gpr_tools/gpr_tools}"
BENCH="${BENCH:-$BUILD_DIR/source/app/bench_fused/bench_fused}"
MOV="${MOV:-$REPO/tools/gpr2prores/gpr_mov_tool}"
PY="${PY:-$(command -v python3 || true)}"
DNGC="${DNGC:-/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter}"

for tool in "$GTOOLS" "$BENCH" "$MOV"; do
    [ -x "$tool" ] || { echo "ERROR: $tool not built" >&2; exit 2; }
done

if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
    GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-$GPR_EXTERNAL_ROOT/artifacts}"
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
TMPDIR="$GPR_TMPDIR"
WORK_PARENT="${WORK:-$GPR_ARTIFACT_ROOT/intermediate}"
mkdir -p "$WORK_PARENT" "$TMPDIR"
WORK="$(mktemp -d "$WORK_PARENT/fixture.XXXXXX")"
cleanup_run() {
    if [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" = 1 ]; then
        printf 'Scratch retained: %s\n' "$WORK"
    else
        rm -rf -- "$WORK"
    fi
}
trap cleanup_run EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Step 1 — collect source files, pick the FRAMES-frame slice
say() { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }

say "Source: $SRC"
SOURCES=()
while IFS= read -r -d '' path; do
    SOURCES+=("$path")
done < <("$PY" - "$SRC" <<'PYEOF'
import sys
from pathlib import Path
root = Path(sys.argv[1]).resolve()
for path in sorted(root.rglob('*')):
    if path.is_file() and len(path.relative_to(root).parts) <= 2 and path.suffix.lower() in {'.dng', '.nef', '.cr3', '.arw'}:
        sys.stdout.buffer.write(bytes(path) + b'\0')
PYEOF
)
N=${#SOURCES[@]}
echo "  total source frames: $N"
if [ "$N" -lt "$FRAMES" ]; then
    echo "ERROR: source has $N frames, need $FRAMES" >&2; exit 1
fi

if [ "$START" = "mid" ]; then
    OFFSET=$(( (N - FRAMES) / 2 ))
else
    OFFSET="$START"
fi
SELECTED=("${SOURCES[@]:OFFSET:FRAMES}")
echo "  selected $FRAMES starting at index $OFFSET"

# Step 2 — convert to DNG if needed
EXT_LC=$("$PY" -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).suffix[1:].lower())' "${SELECTED[0]}")
DNG_DIR="$WORK/dng_files"
mkdir -p "$DNG_DIR"
if [ "$EXT_LC" = "dng" ]; then
    say "Source is DNG; symlinking"
    index=0
    for f in "${SELECTED[@]}"; do
        ln -s "$f" "$DNG_DIR/$(printf 'frame_%06d.dng' "$index")"
        index=$((index+1))
    done
else
    say "Source is $EXT_LC; converting to DNG via Adobe DNG Converter"
    [ -x "$DNGC" ] || { echo "ERROR: $DNGC not found" >&2; exit 2; }
    "$DNGC" -c -d "$DNG_DIR" "${SELECTED[@]}" 2>&1 | tail -3
fi
DNG_COUNT=$(ls "$DNG_DIR"/*.dng 2>/dev/null | wc -l | tr -d ' ')
echo "  DNGs ready: $DNG_COUNT"
[ "$DNG_COUNT" -eq "$FRAMES" ] || { echo "ERROR: expected $FRAMES DNGs, got $DNG_COUNT" >&2; exit 1; }

# Step 3 — DNG → raw bayer plane → FUSED .gpr (with LL)
say "Extract raw bayer + FUSED-encode each frame"
GPR_DIR="$WORK/gpr_files"
mkdir -p "$GPR_DIR"
i=0
for dng in "$DNG_DIR"/*.dng; do
    raw="$WORK/$(basename "$dng" .dng).raw"
    "$PY" - "$dng" "$raw" <<'PYEOF'
import sys, rawpy
r = rawpy.imread(sys.argv[1])
r.raw_image.copy().astype('<u2').tofile(sys.argv[2])
r.close()
PYEOF
    H=$("$PY" -c 'import rawpy,sys; r = rawpy.imread(sys.argv[1]); print(r.raw_image.shape[0]); r.close()' "$dng")
    W=$("$PY" -c 'import rawpy,sys; r = rawpy.imread(sys.argv[1]); print(r.raw_image.shape[1]); r.close()' "$dng")
    # Multi-level wavelet compression with channel-space decimate=2
    # provides the half-resolution topology expected by the CNN.
    #
    # Override: pass GPR_FIXTURE_LEGACY=1 to fall back to the older
    # single-level + LL + decimate path if a fixture needs to match an
    # older measurement.
    if [ "${GPR_FIXTURE_LEGACY:-0}" = "1" ]; then
        FUSED_QUALITY="$QUALITY" GPR_INCLUDE_LL=1 GPR_COL_DECIMATE=2 GPR_ROW_DECIMATE=2 \
            GPR_BENCH_DUMP="$GPR_DIR/$(printf 'frame_%04d.gpr' $i)" \
            "$BENCH" "$raw" "$W" "$H" 2 >/dev/null 2>&1
    else
        FUSED_QUALITY="$QUALITY" FUSED_MULTI_LEVEL=1 GPR_COL_DECIMATE=2 GPR_ROW_DECIMATE=2 \
            GPR_BENCH_DUMP="$GPR_DIR/$(printf 'frame_%04d.gpr' $i)" \
            "$BENCH" "$raw" "$W" "$H" 2 >/dev/null 2>&1
    fi
    rm -f "$raw"
    i=$((i+1))
done
GPR_COUNT=$(ls "$GPR_DIR"/*.gpr 2>/dev/null | wc -l | tr -d ' ')
echo "  FUSED .gpr files: $GPR_COUNT"

# Step 4 — pack into .mov
say "Pack into .mov container"
"$MOV" pack "$GPR_DIR" "$OUT" --fps "$FPS" 2>&1 | tail -2
SIZE=$(du -h "$OUT" 2>/dev/null | awk '{print $1}')
echo "  output: $OUT ($SIZE)"

say "Done"
echo "    To smoke-test playback:"
echo "    tools/gpr2prores/gpr2prores --ckpt \$GPR_ARTIFACT_ROOT/weights/F_ane_1x_weights_metal \\"
echo "        --cnn-backend mpsgraph --cnn-scale 1x \\"
echo "        --demosaic metal-bilinear --out-resolution uhd \\"
echo "        --meta-dng <one of the source DNGs> \\"
echo "        $OUT \$TMPDIR/playback_out.mov"
