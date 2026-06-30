#!/bin/bash
#
# Runs the GPR<->DNG<->RAW conversion pipeline against one or more GoPro GPR
# files, writing each file's output into its own subfolder of out/.
#
# Usage:
#   ./gopro.sh                          # process every *.GPR in src/
#   ./gopro.sh src/HERO13.GPR ...       # process only the given file(s)
#
# Override the gpr_tools binary location with GPR_TOOLS=/path/to/gpr_tools.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

GPR_TOOLS="${GPR_TOOLS:-$SCRIPT_DIR/../../gpr-src/build/source/app/gpr_tools/Debug/gpr_tools}"
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
    SOURCES=(src/*.GPR)
fi

if [ "${#SOURCES[@]}" -eq 0 ]; then
    echo "error: no GPR files to process (none given, and none found under src/)" >&2
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

    # GPR -> GPR (re-encode, refreshing the embedded preview)
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/GPR_FROM_GPR.GPR"

    # GPR -> DNG (also dumps metadata)
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/DNG_FROM_GPR.DNG" -d > "$OUT_DIR/$NAME.JSON"
    # GPR -> RAW
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/RAW_FROM_GPR.RAW"
    # GPR -> PPM
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/PPM_FROM_GPR.PPM"
    # GPR -> JPG
    "$GPR_TOOLS" -i "$SOURCE" -o "$OUT_DIR/JPG_FROM_GPR.JPG"

    # DNG -> GPR
    "$GPR_TOOLS" -i "$OUT_DIR/DNG_FROM_GPR.DNG" -o "$OUT_DIR/GPR_FROM_DNG.GPR"
    # DNG -> RAW
    "$GPR_TOOLS" -i "$OUT_DIR/DNG_FROM_GPR.DNG" -o "$OUT_DIR/RAW_FROM_DNG.RAW"

    # TODO - does not work - PPM/JPG output can only be generated from GPR
    # DNG -> PPM
    # "$GPR_TOOLS" -i "$OUT_DIR/DNG_FROM_GPR.DNG" -o "$OUT_DIR/PPM_FROM_DNG.PPM"
    # DNG -> JPG
    # "$GPR_TOOLS" -i "$OUT_DIR/DNG_FROM_GPR.DNG" -o "$OUT_DIR/JPG_FROM_DNG.JPG"

    echo
done
