#!/bin/bash
# GPR Batch Encoder — Production CLI
#
# Batch-encodes images with noise-aware compression + ANS entropy coding.
# Supports GoPro GPR, DNG, and Nikon NEF (via Adobe DNG Converter).
#
# Usage:
#   gpr_batch.sh <input_dir> <output_dir> [options]
#
# Options:
#   -j N         Parallel jobs (default: CPU count / 2)
#   -q N         Quality preset 0-8 (default: 3)
#   -D           Enable noise-aware denoise (recommended)
#   -A           Enable ANS entropy coding (recommended for 14-bit)
#   --report     Generate compression report CSV
#   --dry-run    Show what would be processed without encoding
#
# Examples:
#   gpr_batch.sh /Volumes/Photos/2025 ./compressed -D -A --report
#   gpr_batch.sh ./raw_files ./gpr_files -q 5 -D -j 8

set -euo pipefail 2>/dev/null || true

# Defaults
JOBS=$(( $(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) / 2 ))
QUALITY=3
DENOISE=""
ANS=""
REPORT=""
DRY_RUN=0

# Find gpr_tools
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GPR_TOOLS="${SCRIPT_DIR}/../../../build/source/app/gpr_tools/gpr_tools"
# Resolve to absolute path
GPR_TOOLS="$(python3 -c "import os; print(os.path.realpath('$GPR_TOOLS'))" 2>/dev/null || readlink -f "$GPR_TOOLS" 2>/dev/null || echo "$GPR_TOOLS")"
if [ ! -f "$GPR_TOOLS" ]; then
    echo "Error: gpr_tools not found at $GPR_TOOLS"
    echo "Build first: cd build && cmake .. && make"
    exit 1
fi
DNG_CONVERTER="/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter"

# Parse arguments
INPUT_DIR=""
OUTPUT_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -q) QUALITY="$2"; shift 2 ;;
        -D) DENOISE="-D 1"; shift ;;
        -A) ANS="-A 1"; shift ;;
        --report) REPORT=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help)
            head -20 "$0" | grep "^#" | sed 's/^# \?//'
            exit 0 ;;
        *)
            if [ -z "$INPUT_DIR" ]; then INPUT_DIR="$1"
            elif [ -z "$OUTPUT_DIR" ]; then OUTPUT_DIR="$1"
            fi
            shift ;;
    esac
done

if [ -z "$INPUT_DIR" ] || [ -z "$OUTPUT_DIR" ]; then
    echo "Usage: gpr_batch.sh <input_dir> <output_dir> [options]"
    echo "Run with --help for details."
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# Collect input files
INPUTS=()
while IFS= read -r f; do
    INPUTS+=("$f")
done < <(find "$INPUT_DIR" -type f \( \
    -name "*.GPR" -o -name "*.gpr" -o \
    -name "*.DNG" -o -name "*.dng" -o \
    -name "*.NEF" -o -name "*.nef" \
\) | sort)

TOTAL=${#INPUTS[@]}
if [ $TOTAL -eq 0 ]; then
    echo "No supported files found in $INPUT_DIR"
    exit 1
fi

echo "╔══════════════════════════════════════════╗"
echo "║  GPR Batch Encoder                       ║"
echo "╠══════════════════════════════════════════╣"
echo "║  Input:    $INPUT_DIR"
echo "║  Output:   $OUTPUT_DIR"
echo "║  Files:    $TOTAL"
echo "║  Quality:  Q$QUALITY"
echo "║  Denoise:  $([ -n "$DENOISE" ] && echo "YES" || echo "no")"
echo "║  ANS:      $([ -n "$ANS" ] && echo "YES" || echo "no")"
echo "║  Parallel: $JOBS jobs"
echo "╚══════════════════════════════════════════╝"
echo ""

if [ $DRY_RUN -eq 1 ]; then
    for f in "${INPUTS[@]}"; do echo "  Would process: $(basename "$f")"; done
    exit 0
fi

# Report CSV
REPORT_FILE="$OUTPUT_DIR/compression_report.csv"
if [ -n "$REPORT" ]; then
    echo "filename,input_size,output_size,ratio,format" > "$REPORT_FILE"
fi

# Process function
process_one() {
    local INPUT="$1"
    local NAME=$(basename "$INPUT")
    local BASE="${NAME%.*}"
    local EXT="${NAME##*.}"
    local EXT_LOWER=$(echo "$EXT" | tr '[:upper:]' '[:lower:]')
    local TEMP_DNG=""
    local ENCODE_INPUT="$INPUT"

    # Convert NEF to DNG if needed
    if [ "$EXT_LOWER" = "nef" ]; then
        if [ ! -f "$DNG_CONVERTER" ]; then
            echo "SKIP $NAME (no Adobe DNG Converter for NEF)" >&2
            return
        fi
        TEMP_DNG="$OUTPUT_DIR/.tmp_${BASE}.dng"
        cp "$INPUT" "$OUTPUT_DIR/.tmp_${BASE}.${EXT}"
        "$DNG_CONVERTER" -c -d "$OUTPUT_DIR" "$OUTPUT_DIR/.tmp_${BASE}.${EXT}" >/dev/null 2>&1
        rm -f "$OUTPUT_DIR/.tmp_${BASE}.${EXT}"
        if [ -f "$TEMP_DNG" ]; then
            ENCODE_INPUT="$TEMP_DNG"
        else
            echo "FAIL $NAME (DNG conversion failed)" >&2
            return
        fi
    fi

    # For GPR input: GPR → DNG → GPR (re-encode with new settings)
    local OUTPUT="$OUTPUT_DIR/${BASE}.GPR"
    if [ "$EXT_LOWER" = "gpr" ]; then
        local TEMP_DNG2="$OUTPUT_DIR/.tmp_${BASE}_src.DNG"
        $GPR_TOOLS -i "$ENCODE_INPUT" -o "$TEMP_DNG2" 2>/dev/null
        $GPR_TOOLS -i "$TEMP_DNG2" -o "$OUTPUT" -q $QUALITY $DENOISE $ANS 2>/dev/null
        rm -f "$TEMP_DNG2"
    else
        $GPR_TOOLS -i "$ENCODE_INPUT" -o "$OUTPUT" -q $QUALITY $DENOISE $ANS 2>/dev/null
    fi

    # Cleanup temp DNG
    [ -n "$TEMP_DNG" ] && rm -f "$TEMP_DNG"

    if [ -f "$OUTPUT" ]; then
        local IN_SZ=$(stat -f%z "$INPUT" 2>/dev/null || stat -c%s "$INPUT" 2>/dev/null)
        local OUT_SZ=$(stat -f%z "$OUTPUT" 2>/dev/null || stat -c%s "$OUTPUT" 2>/dev/null)
        local RATIO=$(python3 -c "print(f'{${IN_SZ}/${OUT_SZ}:.1f}x')" 2>/dev/null || echo "?x")
        echo "OK  $NAME → ${BASE}.GPR ($RATIO)"

        if [ -n "$REPORT" ]; then
            echo "$NAME,$IN_SZ,$OUT_SZ,$RATIO,$EXT_LOWER" >> "$REPORT_FILE"
        fi
    else
        echo "FAIL $NAME" >&2
    fi
}

export -f process_one 2>/dev/null
export GPR_TOOLS QUALITY DENOISE ANS DNG_CONVERTER OUTPUT_DIR REPORT REPORT_FILE

# Process with progress
START=$(date +%s)
DONE=0
FAIL=0

# Use GNU parallel if available, otherwise xargs
if command -v parallel &>/dev/null; then
    printf '%s\n' "${INPUTS[@]}" | parallel -j $JOBS process_one {}
else
    for INPUT in "${INPUTS[@]}"; do
        process_one "$INPUT" &
        DONE=$((DONE + 1))
        # Limit parallel jobs
        if [ $((DONE % JOBS)) -eq 0 ]; then wait; fi
    done
    wait
fi

END=$(date +%s)
ELAPSED=$((END - START))
PROCESSED=$(find "$OUTPUT_DIR" -name "*.GPR" | wc -l | tr -d ' ')

echo ""
echo "════════════════════════════════════════════"
echo "Done: $PROCESSED/$TOTAL files in ${ELAPSED}s"
if [ -n "$REPORT" ]; then
    echo "Report: $REPORT_FILE"
    # Summary stats
    python3 -c "
import csv
with open('$REPORT_FILE') as f:
    rows = list(csv.DictReader(f))
if rows:
    total_in = sum(int(r['input_size']) for r in rows)
    total_out = sum(int(r['output_size']) for r in rows)
    print(f'Total: {total_in/1e6:.0f}MB → {total_out/1e6:.0f}MB ({total_in/total_out:.1f}x)')
" 2>/dev/null
fi
echo "════════════════════════════════════════════"
