#!/bin/bash
#
# Runs the GPR<->DNG<->RAW conversion pipeline against every capture file in an
# input folder, writing each file's output into its own subfolder of out/.
#
# Files are found recursively and dispatched by extension:
#   *.GPR -> RunGoproPipeline
#   *.DNG -> RunIphonePipeline   (BGGR DNGs such as iPhone raws)
# Any other file is skipped.
#
# Usage:
#   ./scripts/test_conversions.sh ./data/samples
#
# Output is written to ./out/<name>/ under the current directory (override with
# OUT_ROOT=/path). The JPEG embedded by the --preview steps is decoded from the
# first GPR found (a 16:1 render) unless PREVIEW_FILE=/path names one. Override
# the gpr_tools binary with GPR_TOOLS=/path.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <input-folder>" >&2
    exit 2
fi

INPUT_DIR="$1"
if [ ! -d "$INPUT_DIR" ]; then
    echo "error: not a directory: $INPUT_DIR" >&2
    exit 1
fi
INPUT_DIR="$(cd "$INPUT_DIR" && pwd)"

OUT_ROOT="${OUT_ROOT:-$PWD/out}"

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

# iPhone DNGs: convert to GPR both directly and via a RAW round-trip, then
# decode the round-tripped GPR back out to DNG/PPM/JPG. Apple encodes in BGGR
# format - when we shift by one column, it becomes GRBG.
RunIphonePipeline()
{
    local SOURCE="$1" OUT_DIR="$2" NAME="$3"

    # Direct DNG -> GPR, for quick comparison against the RAW round-trip below.
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_DNG.GPR\" --input_skip_cols=1 --input_pixel_format=gbrg12"

    # Same, but written with a .DNG extension (--output_format=gpr forces GPR encoding)
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_DNG.DNG\" --output_format=gpr --input_skip_cols=1 --input_pixel_format=gbrg12"

    # DNG -> GPR with external preview
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_DNG_WITH_PREVIEW.GPR\" --input_skip_cols=1 --input_pixel_format=gbrg12 --preview=\"$PREVIEW\""

    # DNG -> GPR with external preview, written with a .DNG extension
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_DNG_WITH_PREVIEW.DNG\" --output_format=gpr --input_skip_cols=1 --input_pixel_format=gbrg12 --preview=\"$PREVIEW\""

    # DNG -> RAW (also dumps metadata, needed to re-interpret the raw bytes below)
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/RAW_FROM_DNG.RAW\" -d > \"$OUT_DIR/$NAME.JSON\""

    # RAW -> GPR
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/RAW_FROM_DNG.RAW\" -o \"$OUT_DIR/GPR_FROM_RAW.GPR\" -a \"$OUT_DIR/$NAME.JSON\" --input_skip_cols=1 --input_pixel_format=gbrg12"

    # GPR -> DNG
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/GPR_FROM_RAW.GPR\" -o \"$OUT_DIR/DNG_FROM_GPR.DNG\""

    # GPR -> PPM
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/GPR_FROM_RAW.GPR\" -o \"$OUT_DIR/PPM_FROM_GPR-8bits.PPM\" --output_ppm_bits=8 --rgb_resolution=2:1"
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/GPR_FROM_RAW.GPR\" -o \"$OUT_DIR/PPM_FROM_GPR-16bits.PPM\" --output_ppm_bits=16 --rgb_resolution=2:1"

    # GPR -> JPG
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/GPR_FROM_RAW.GPR\" -o \"$OUT_DIR/JPG_FROM_GPR.JPG\" --rgb_resolution=2:1"

    # RAW -> DNG
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/RAW_FROM_DNG.RAW\" -o \"$OUT_DIR/DNG_FROM_RAW.DNG\" -a \"$OUT_DIR/$NAME.JSON\""
}

# GoPro GPRs: re-encode, decode out to DNG/RAW/PPM/JPG, then convert the
# generated DNG back to GPR and RAW.
RunGoproPipeline()
{
    local SOURCE="$1" OUT_DIR="$2" NAME="$3"
    local COMMON_PARAMS=""

    # GPR -> GPR (no --preview: repackages the vc5 bitstream, no preview written)
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_GPR.GPR\"$COMMON_PARAMS"

    # GPR -> GPR with external preview
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/GPR_FROM_GPR_PREV.GPR\" --preview=\"$PREVIEW\"$COMMON_PARAMS"

    # GPR -> DNG (also dumps metadata)
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/DNG_FROM_GPR.DNG\" -d$COMMON_PARAMS > \"$OUT_DIR/$NAME.JSON\""
    # GPR -> RAW
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/RAW_FROM_GPR.RAW\"$COMMON_PARAMS"
    # GPR -> PPM
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/PPM_FROM_GPR.PPM\"$COMMON_PARAMS"
    # GPR -> JPG
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$OUT_DIR/JPG_FROM_GPR.JPG\"$COMMON_PARAMS"

    # DNG -> GPR
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/GPR_FROM_DNG.GPR\"$COMMON_PARAMS"
    # DNG -> RAW
    ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/RAW_FROM_DNG.RAW\"$COMMON_PARAMS"

    # TODO - does not work - PPM/JPG output can only be generated from GPR
    # DNG -> PPM
    # ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/PPM_FROM_DNG.PPM\""
    # DNG -> JPG
    # ExecuteCommand "\"$GPR_TOOLS\" -i \"$OUT_DIR/DNG_FROM_GPR.DNG\" -o \"$OUT_DIR/JPG_FROM_DNG.JPG\""
}

# Xcode layout first, then the single-config Makefile/Ninja layout, then PATH
GPR_TOOLS="${GPR_TOOLS:-$REPO_DIR/build/source/app/gpr_tools/Release/gpr_tools}"
if [ ! -x "$GPR_TOOLS" ]; then
    GPR_TOOLS="$REPO_DIR/build/source/app/gpr_tools/gpr_tools"
fi
if [ ! -x "$GPR_TOOLS" ]; then
    GPR_TOOLS="$(command -v gpr_tools || true)"
fi
if [ -z "$GPR_TOOLS" ] || [ ! -x "$GPR_TOOLS" ]; then
    echo "error: gpr_tools binary not found. Build it, or set GPR_TOOLS=/path/to/gpr_tools" >&2
    exit 1
fi

FILES=()
while IFS= read -r f; do FILES+=("$f"); done < <(find "$INPUT_DIR" -type f \( -iname '*.gpr' -o -iname '*.dng' \) | sort)

rm -rf "$OUT_ROOT"
mkdir -p "$OUT_ROOT"

# The preview JPEG the --preview steps embed: decode one from the first GPR
# unless the caller supplied a file.
PREVIEW="${PREVIEW_FILE:-}"
if [ -z "$PREVIEW" ]; then
    for SOURCE in "${FILES[@]}"; do
        case "$(echo "${SOURCE##*.}" | tr '[:lower:]' '[:upper:]')" in
            GPR)
                PREVIEW="$OUT_ROOT/preview.jpg"
                ExecuteCommand "\"$GPR_TOOLS\" -i \"$SOURCE\" -o \"$PREVIEW\" --rgb_resolution=16:1"
                break ;;
        esac
    done
fi
if [ -z "$PREVIEW" ] || [ ! -f "$PREVIEW" ]; then
    echo "error: no preview image: no GPR to decode one from (set PREVIEW_FILE=/path)" >&2
    exit 1
fi

PROCESSED=0
FAILED=()
for SOURCE in "${FILES[@]}"; do
    [ -f "$SOURCE" ] || continue

    NAME="$(basename "$SOURCE")"
    EXT="$(echo "${NAME##*.}" | tr '[:lower:]' '[:upper:]')"
    NAME="${NAME%.*}"

    case "$EXT" in
        GPR) PIPELINE=RunGoproPipeline ;;
        DNG) PIPELINE=RunIphonePipeline ;;
        *)   continue ;;
    esac

    OUT_DIR="$OUT_ROOT/$NAME"
    mkdir -p "$OUT_DIR"

    echo "== $NAME ($SOURCE) =="
    # Run each file's pipeline in a subshell so a failure (e.g. a gpr_tools
    # crash on one sample) is isolated and the batch continues with the rest.
    if ( "$PIPELINE" "$SOURCE" "$OUT_DIR" "$NAME" ); then
        PROCESSED=$((PROCESSED + 1))
    else
        echo "error: pipeline failed for $NAME (exit $?)" >&2
        FAILED+=("$NAME")
    fi
    echo
done

if [ "$PROCESSED" -eq 0 ] && [ "${#FAILED[@]}" -eq 0 ]; then
    echo "error: no GPR or DNG files found in $INPUT_DIR" >&2
    exit 1
fi

echo "processed $PROCESSED file(s), ${#FAILED[@]} failed"
if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "failed: ${FAILED[*]}" >&2
    exit 1
fi
