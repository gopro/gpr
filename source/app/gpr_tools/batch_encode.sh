#!/bin/bash
# Batch GPR encode with process-level parallelism
# Usage: batch_encode.sh <input_dir> <output_dir> [flags]
# Example: batch_encode.sh /path/to/dngs /path/to/gprs -D 1 -A 1

INPUT_DIR="$1"
OUTPUT_DIR="$2"
shift 2
FLAGS="$@"
JOBS=${BATCH_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}

GPR_TOOLS="$(dirname "$0")/../../build/source/app/gpr_tools/gpr_tools"

mkdir -p "$OUTPUT_DIR"

echo "Batch encode: $INPUT_DIR → $OUTPUT_DIR"
echo "Flags: $FLAGS"
echo "Parallel jobs: $JOBS"
echo ""

# Count input files
INPUTS=($(find "$INPUT_DIR" -name "*.DNG" -o -name "*.dng" -o -name "*.GPR" -o -name "*.gpr" | sort))
TOTAL=${#INPUTS[@]}
echo "Found $TOTAL files to process"

START=$(date +%s)

# Process in parallel using xargs
printf '%s\n' "${INPUTS[@]}" | xargs -P "$JOBS" -I {} bash -c '
    INPUT="{}"
    NAME=$(basename "$INPUT")
    NAME="${NAME%.*}"
    OUTPUT="'"$OUTPUT_DIR"'/${NAME}.GPR"

    '"$GPR_TOOLS"' -i "$INPUT" -o "$OUTPUT" '"$FLAGS"' 2>/dev/null
    if [ $? -eq 0 ]; then
        echo "OK: $NAME"
    else
        echo "FAIL: $NAME" >&2
    fi
'

END=$(date +%s)
ELAPSED=$((END - START))
DONE=$(find "$OUTPUT_DIR" -name "*.GPR" | wc -l | tr -d ' ')
RATE=$(python3 -c "print(f'{${DONE}/${ELAPSED:.1f} if $ELAPSED > 0 else 0:.1f}')" 2>/dev/null)

echo ""
echo "Done: $DONE/$TOTAL in ${ELAPSED}s (${RATE} files/s)"
