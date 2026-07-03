#!/bin/bash
#
# Runs the GPR<->DNG<->RAW conversion pipeline against one or more iPhone DNG
# files, writing each file's output into its own subfolder of out/.
#
# Usage:
#   ./iphone.sh                          # process every *.DNG in src/
#   ./iphone.sh src/LAND-12MP.DNG ...    # process only the given file(s)
#
# Override the gpr_tools binary location with GPR_TOOLS=/path/to/gpr_tools.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

GPR_TOOLS="${GPR_TOOLS:-$SCRIPT_DIR/../../../build/source/app/gpr_tools/Debug/gpr_tools}"
if [ ! -x "$GPR_TOOLS" ]; then
    GPR_TOOLS="$(command -v gpr_tools || true)"
fi
if [ -z "$GPR_TOOLS" ] || [ ! -x "$GPR_TOOLS" ]; then
    echo "error: gpr_tools binary not found. Build it, or set GPR_TOOLS=/path/to/gpr_tools" >&2
    exit 1
fi

if [ "$#" -gt 0 ]; then
    SOURCES=("$@")
else
    SOURCES=(src/*.DNG)
fi

if [ "${#SOURCES[@]}" -eq 0 ]; then
    echo "error: no DNG files to process (none given, and none found under src/)" >&2
    exit 1
fi

rm -rf out
mkdir -p out

for SOURCE in "${SOURCES[@]}"; do
    NAME="$(basename "$SOURCE")"
    NAME="${NAME%.*}"
    OUT_DIR="out/$NAME"
    mkdir -p "$OUT_DIR"

    echo "== $NAME ($SOURCE) =="

    # Direct DNG -> GPR, for quick comparison against the RAW round-trip below.
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/GPR_FROM_DNG.GPR"

    # DNG -> GPR with external preview
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/GPR_FROM_DNG_PREV.GPR" --preview_file_path=../lena.jpg

    # DNG -> RAW (also dumps metadata, needed to re-interpret the raw bytes below)
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/RAW_FROM_DNG.RAW" -d > "$OUT_DIR/$NAME.JSON"

    # RAW -> GPR (Apple iPhone encodes in BGGR format - when we shift by one
    # column, it becomes GRBG)
    "$GPR_TOOLS" -i "$OUT_DIR/RAW_FROM_DNG.RAW" -o "$OUT_DIR/GPR_FROM_RAW.GPR" \
        -a "$OUT_DIR/$NAME.JSON" --input_skip_cols=1 --input_pixel_format=gbrg12

    # GPR -> DNG
    "$GPR_TOOLS" -i "$OUT_DIR/GPR_FROM_RAW.GPR" -o "$OUT_DIR/DNG_FROM_GPR.DNG"

    # GPR -> PPM
    "$GPR_TOOLS" -i "$OUT_DIR/GPR_FROM_RAW.GPR" -o "$OUT_DIR/PPM_FROM_GPR-8bits.PPM" \
        --output_ppm_bits=8 --rgb_resolution=2:1
    "$GPR_TOOLS" -i "$OUT_DIR/GPR_FROM_RAW.GPR" -o "$OUT_DIR/PPM_FROM_GPR-16bits.PPM" \
        --output_ppm_bits=16 --rgb_resolution=2:1

    # GPR -> JPG
    "$GPR_TOOLS" -i "$OUT_DIR/GPR_FROM_RAW.GPR" -o "$OUT_DIR/JPG_FROM_GPR.JPG" \
        --rgb_resolution=2:1

    # RAW -> DNG
    "$GPR_TOOLS" -i "$OUT_DIR/RAW_FROM_DNG.RAW" -o "$OUT_DIR/DNG_FROM_RAW.DNG" \
        -a "$OUT_DIR/$NAME.JSON"

    echo
done
