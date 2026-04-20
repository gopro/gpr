#!/bin/bash
#
# Noise-Transparent Compression Test Suite
#
# Tests the complete pipeline: noise estimation → separation → compression →
# decompression → noise reconstruction → fidelity verification
#
# Usage: ./test_noise_compression.sh <path_to_gpr_tools> [input.GPR]
#

GPR_TOOLS="${1:-../../build/source/app/gpr_tools/gpr_tools}"
INPUT="${2:-../samples/Hero6/GOPR0024.GPR}"
ANALYZE="${3:-../../build/analyze_raw}"

if [ ! -f "$GPR_TOOLS" ]; then
    echo "Error: gpr_tools not found at $GPR_TOOLS"
    echo "Build first: cd build && cmake .. && make"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

echo "================================================================"
echo "  Noise-Transparent Compression Test"
echo "  Input: $INPUT"
echo "================================================================"
echo ""

# Get image parameters
echo "--- Decoding to RAW ---"
"$GPR_TOOLS" -i "$INPUT" -o "$TMPDIR/original.RAW" 2>/dev/null
"$GPR_TOOLS" -i "$INPUT" -d 1 > "$TMPDIR/params.TXT" 2>/dev/null

WIDTH=$(grep '"input_width"' "$TMPDIR/params.TXT" | grep -o '[0-9]*')
HEIGHT=$(grep '"input_height"' "$TMPDIR/params.TXT" | grep -o '[0-9]*')
NPIXELS=$((WIDTH * HEIGHT))
ORIG_SIZE=$(stat -f%z "$TMPDIR/original.RAW" 2>/dev/null || stat -c%s "$TMPDIR/original.RAW" 2>/dev/null)

echo "  Dimensions: ${WIDTH}x${HEIGHT}"
echo "  Raw size: $ORIG_SIZE bytes"
echo ""

# Noise analysis (if analyze_raw is available)
if [ -f "$ANALYZE" ]; then
    echo "--- Noise Analysis ---"
    "$ANALYZE" "$TMPDIR/original.RAW" "$WIDTH" "$HEIGHT" 14 2>/dev/null | grep -E "Read Noise|noise_scale|noise_offset|Dark sigma|FPN|entropy"
    echo ""
fi

# Test matrix
echo "--- Compression Comparison ---"
echo ""
printf "%-25s %10s %6s %10s %10s\n" "Mode" "GPR Size" "Ratio" "PSNR" "RMSE"
printf "%-25s %10s %6s %10s %10s\n" "----" "--------" "-----" "----" "----"

for Q in 0 2 4 6; do
    # Normal (baseline)
    "$GPR_TOOLS" -i "$TMPDIR/original.RAW" -o "$TMPDIR/n_q${Q}.GPR" \
        -a "$TMPDIR/params.TXT" -q $Q 2>/dev/null
    "$GPR_TOOLS" -i "$TMPDIR/n_q${Q}.GPR" -o "$TMPDIR/n_q${Q}.RAW" 2>/dev/null
    N_SZ=$(stat -f%z "$TMPDIR/n_q${Q}.GPR" 2>/dev/null || stat -c%s "$TMPDIR/n_q${Q}.GPR" 2>/dev/null)
    N_RATIO=$(echo "scale=1; $ORIG_SIZE / $N_SZ" | bc)

    if [ -f /tmp/psnr16 ]; then
        N_METRICS=$(/tmp/psnr16 "$TMPDIR/original.RAW" "$TMPDIR/n_q${Q}.RAW" $NPIXELS 2>/dev/null)
        N_PSNR=$(echo "$N_METRICS" | sed 's/.*PSNR=\([^ ]*\).*/\1/')
        N_RMSE=$(echo "$N_METRICS" | sed 's/.*RMSE=\([^ ]*\).*/\1/')
    else
        N_PSNR="N/A"
        N_RMSE="N/A"
    fi
    printf "%-25s %10s %5sx %10s %10s\n" "Normal Q$Q" "$N_SZ" "$N_RATIO" "$N_PSNR" "$N_RMSE"

    # Denoised at various strengths
    for S in 10 30; do
        "$GPR_TOOLS" -i "$TMPDIR/original.RAW" -o "$TMPDIR/d_q${Q}_s${S}.GPR" \
            -a "$TMPDIR/params.TXT" -q $Q --Denoise -N $S 2>/dev/null
        "$GPR_TOOLS" -i "$TMPDIR/d_q${Q}_s${S}.GPR" -o "$TMPDIR/d_q${Q}_s${S}.RAW" 2>/dev/null
        D_SZ=$(stat -f%z "$TMPDIR/d_q${Q}_s${S}.GPR" 2>/dev/null || stat -c%s "$TMPDIR/d_q${Q}_s${S}.GPR" 2>/dev/null)
        D_RATIO=$(echo "scale=1; $ORIG_SIZE / $D_SZ" | bc)

        if [ -f /tmp/psnr16 ]; then
            D_METRICS=$(/tmp/psnr16 "$TMPDIR/original.RAW" "$TMPDIR/d_q${Q}_s${S}.RAW" $NPIXELS 2>/dev/null)
            D_PSNR=$(echo "$D_METRICS" | sed 's/.*PSNR=\([^ ]*\).*/\1/')
            D_RMSE=$(echo "$D_METRICS" | sed 's/.*RMSE=\([^ ]*\).*/\1/')
        else
            D_PSNR="N/A"
            D_RMSE="N/A"
        fi

        SAVINGS=$(echo "scale=1; (1 - $D_SZ * 1.0 / $N_SZ) * 100" | bc)
        printf "%-25s %10s %5sx %10s %10s  (-%s%%)\n" \
            "  Denoise Q$Q N$S" "$D_SZ" "$D_RATIO" "$D_PSNR" "$D_RMSE" "$SAVINGS"
    done
    echo ""
done

echo "================================================================"
echo "  Interpretation:"
echo "  - 'Ratio' = raw_size / compressed_size (higher = better compression)"
echo "  - 'PSNR' = fidelity vs original (higher = closer to original)"
echo "  - PSNR drop indicates noise energy identified and separated"
echo "  - Savings% shows compression improvement from noise separation"
echo "================================================================"
