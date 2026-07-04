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

# Print the command, then run it. Takes the full command as one string so that
# redirections are part of it and the echoed line matches what actually ran,
# ready to be copy-pasted when re-running a single step by hand.
# Terminates the whole script if the command fails, reporting the failed
# command and its exit status.
ExecuteCommand()
{
    echo "\$ $1"

    local status=0
    eval "$1" || status=$?

    if [ "$status" -ne 0 ]; then
        echo "error: command failed with exit status $status: $1" >&2
        exit "$status"
    fi
}

GPR_TOOLS="${GPR_TOOLS:-$SCRIPT_DIR/../../../build/source/app/gpr_tools/Release/gpr_tools}"
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
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_GPR.GPR\""

    # DNG -> GPR with external preview
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_GPR_PREV.GPR\" --preview_file_path=../lena.jpg"

    # GPR -> DNG (also dumps metadata)
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/DNG_FROM_GPR.DNG\" -d > \"$OUT_DIR/$NAME.JSON\""
    # GPR -> RAW
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/RAW_FROM_GPR.RAW\""
    # GPR -> PPM
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/PPM_FROM_GPR.PPM\""
    # GPR -> JPG
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/JPG_FROM_GPR.JPG\""

    # DNG -> GPR
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/GPR_FROM_DNG.GPR\""
    # DNG -> RAW
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/RAW_FROM_DNG.RAW\""

    # TODO - does not work - PPM/JPG output can only be generated from GPR
    # DNG -> PPM
    # ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/PPM_FROM_DNG.PPM\""
    # DNG -> JPG
    # ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/JPG_FROM_DNG.JPG\""

    echo
done
