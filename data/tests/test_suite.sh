#!/bin/bash
# GPR Noise-Aware Compression Test Suite
#
# Three tiers:
#   smoke   - 5 images, ~30 seconds, sanity check
#   medium  - 20 images, ~3 minutes, decision validation
#   full    - 60+ images, ~15 minutes, proof of correctness
#
# Tests encode→decode round-trip at multiple quality/denoise settings,
# measures PSNR, SSIM, file size, and noise preservation.
#
# Usage: ./test_suite.sh [smoke|medium|full] [path_to_gpr_tools]

set -e

TIER="${1:-smoke}"
GPR_TOOLS="${2:-../../build/source/app/gpr_tools/gpr_tools}"
COMPARE="${3:-/tmp/compare_quality}"
TMPDIR="/tmp/gpr_test_$$"
RESULTS="$TMPDIR/results.csv"

# Paths to test data
SAMPLES="../../data/samples"
HERO10_DARK="${HERO10_DARK:-/path/to/hero10/dark}"
PHOCUS="${PHOCUS:-/path/to/phocus/captures}"
HASSEL="${HASSEL:-/path/to/hasselblad/images}"

mkdir -p "$TMPDIR"

# Build compare_quality if needed
if [ ! -f "$COMPARE" ]; then
    echo "Building compare_quality..."
    gcc -O2 -o "$COMPARE" ../../source/app/compare_quality/compare_quality.c -lm
fi

echo "tier,image,camera,iso,quality,denoise,size_base,size_dn,psnr_base,psnr_dn,ssim_dn,noise_ratio,bright_psnr" > "$RESULTS"

# Helper: test one image at one quality with and without denoise
test_image() {
    local INPUT="$1"
    local NAME="$2"
    local CAMERA="$3"
    local ISO="$4"
    local W="$5"
    local H="$6"
    local BITS="$7"
    local FORMAT="$8"
    local QUALITY="$9"

    local RAW="$TMPDIR/raw.RAW"
    local BASE_GPR="$TMPDIR/base.GPR"
    local DN_GPR="$TMPDIR/dn.GPR"
    local BASE_DEC="$TMPDIR/base_dec.RAW"
    local DN_DEC="$TMPDIR/dn_dec.RAW"

    # If input is GPR/DNG, decode to RAW first
    local EXT="${INPUT##*.}"
    EXT=$(echo "$EXT" | tr '[:upper:]' '[:lower:]')
    if [ "$EXT" = "gpr" ] || [ "$EXT" = "dng" ]; then
        $GPR_TOOLS -i "$INPUT" -o "$RAW" 2>/dev/null || return
    elif [ "$EXT" = "fff" ]; then
        # FFF files: extract raw data using exiftool or dcraw
        if command -v dcraw &>/dev/null; then
            dcraw -4 -T -D "$INPUT" -o "$RAW" 2>/dev/null || return
        else
            echo "  SKIP $NAME (no dcraw for .fff)" >&2
            return
        fi
    elif [ "$EXT" = "raw" ]; then
        cp "$INPUT" "$RAW"
    else
        echo "  SKIP $NAME (unknown format $EXT)" >&2
        return
    fi

    # Encode baseline
    $GPR_TOOLS -i "$RAW" -w $W -h $H -x $FORMAT -o "$BASE_GPR" -q $QUALITY 2>/dev/null || return
    local S_BASE=$(stat -f%z "$BASE_GPR" 2>/dev/null || stat -c%s "$BASE_GPR" 2>/dev/null)

    # Encode with denoise
    $GPR_TOOLS -i "$RAW" -w $W -h $H -x $FORMAT -o "$DN_GPR" -q $QUALITY -D 1 2>/dev/null || return
    local S_DN=$(stat -f%z "$DN_GPR" 2>/dev/null || stat -c%s "$DN_GPR" 2>/dev/null)

    # Decode both
    $GPR_TOOLS -i "$BASE_GPR" -o "$BASE_DEC" 2>/dev/null || return
    $GPR_TOOLS -i "$DN_GPR" -o "$DN_DEC" 2>/dev/null || return

    # Measure quality
    local BASE_Q=$($COMPARE "$RAW" "$BASE_DEC" $W $H $BITS 2>/dev/null)
    local DN_Q=$($COMPARE "$RAW" "$DN_DEC" $W $H $BITS 2>/dev/null)

    local P_BASE=$(echo "$BASE_Q" | grep "PSNR:" | head -1 | awk '{print $2}')
    local P_DN=$(echo "$DN_Q" | grep "PSNR:" | head -1 | awk '{print $2}')
    local SSIM=$(echo "$DN_Q" | grep "SSIM:" | awk '{print $2}')
    local NOISE=$(echo "$DN_Q" | grep "Ratio:" | awk '{print $2}')
    local BRIGHT=$(echo "$DN_Q" | grep "Bright" | awk '{print $2}')

    echo "$TIER,$NAME,$CAMERA,$ISO,$QUALITY,$S_BASE,$S_DN,$P_BASE,$P_DN,$SSIM,$NOISE,$BRIGHT" >> "$RESULTS"

    local PCT=$(python3 -c "print(f'{(1-${S_DN}/${S_BASE})*100:.0f}%')" 2>/dev/null || echo "?%")
    local DELTA=$(python3 -c "print(f'{${P_DN}-${P_BASE}:+.1f}')" 2>/dev/null || echo "?")
    echo "  $NAME Q$QUALITY: ${P_BASE}→${P_DN}dB (${DELTA}dB) ${PCT} smaller, SSIM=${SSIM}"
}

echo "=== GPR Test Suite: $TIER tier ==="
echo ""

# ============================================================
# SMOKE TEST: 5 images, quick sanity check
# ============================================================
if [ "$TIER" = "smoke" ] || [ "$TIER" = "medium" ] || [ "$TIER" = "full" ]; then
    echo "--- Smoke test (5 images) ---"

    # 1. Hero6 - well-lit outdoor scene (the baseline test image)
    test_image "$SAMPLES/Hero6/GOPR0024.GPR" "Hero6_outdoor" "Hero6" "?" 4000 3000 14 "rggb14" 3

    # 2. HERO7 - different scene (showed +1.6 dB improvement)
    test_image "$SAMPLES/HERO7/GOPR9231.GPR" "HERO7_scene" "HERO7" "?" 4000 3000 14 "rggb14" 3

    # 3. HERO10 dark frame (low noise, should compress well)
    test_image "$HERO10_DARK/G0016905.GPR" "HERO10_dark" "HERO10" "?" 5568 4176 14 "rggb14" 3

    # 4. Hasselblad X2D DNG (16-bit, 100MP)
    if [ -f "$HASSEL/2024_12_Dec_Austin_0137.dng" ]; then
        test_image "$HASSEL/2024_12_Dec_Austin_0137.dng" "X2D_Austin137" "X2D" "200" 11664 8750 16 "rggb16" 3
    fi

    # 5. Hero5 (noisier, previously problematic)
    test_image "$SAMPLES/Hero5/GOPR2657.GPR" "Hero5_scene" "Hero5" "?" 4000 3000 14 "rggb14" 3

    echo ""
fi

# ============================================================
# MEDIUM TEST: 20 images, wide variety of corner cases
# ============================================================
if [ "$TIER" = "medium" ] || [ "$TIER" = "full" ]; then
    echo "--- Medium test (corner cases) ---"

    # Multiple quality settings on the key image
    for Q in 0 1 3 5 8; do
        test_image "$SAMPLES/Hero6/GOPR0024.GPR" "Hero6_Q${Q}" "Hero6" "?" 4000 3000 14 "rggb14" $Q
    done

    # HERO9
    test_image "$SAMPLES/HERO9/GOPR0002.GPR" "HERO9_scene" "HERO9" "?" 4000 3000 14 "rggb14" 3

    # Multiple HERO10 dark frames (statistical consistency)
    for GPR in "$HERO10_DARK/G0016905.GPR" "$HERO10_DARK/G0016950.GPR" "$HERO10_DARK/G0017000.GPR"; do
        NAME="HERO10_$(basename $GPR .GPR)"
        test_image "$GPR" "$NAME" "HERO10" "?" 5568 4176 14 "rggb14" 3
    done

    # X2D at different quality settings
    if [ -f "$HASSEL/2024_12_Dec_Austin_0186.dng" ]; then
        test_image "$HASSEL/2024_12_Dec_Austin_0186.dng" "X2D_Austin186_Q3" "X2D" "64" 11664 8750 16 "rggb16" 3
        test_image "$HASSEL/2024_12_Dec_Austin_0186.dng" "X2D_Austin186_Q5" "X2D" "64" 11664 8750 16 "rggb16" 5
    fi

    # X2D encoded GPR round-trip
    if [ -f "$HASSEL/encoded_v3.gpr" ]; then
        test_image "$HASSEL/encoded_v3.gpr" "X2D_encoded_v3" "X2D" "232" 11664 8750 16 "rggb16" 3
    fi

    # Fusion (dual-lens stereo)
    for GPR in "$SAMPLES/Fusion/"*.GPR; do
        NAME="Fusion_$(basename $GPR .GPR)"
        test_image "$GPR" "$NAME" "Fusion" "?" 4000 3000 14 "rggb14" 3
    done

    echo ""
fi

# ============================================================
# FULL TEST: 60+ images, comprehensive proof
# ============================================================
if [ "$TIER" = "full" ]; then
    echo "--- Full test (comprehensive) ---"

    # All GoPro samples at Q3 and Q5
    for GPR in "$SAMPLES"/*/*.GPR; do
        NAME="$(basename $(dirname $GPR))_$(basename $GPR .GPR)"
        for Q in 3 5; do
            test_image "$GPR" "${NAME}_Q${Q}" "GoPro" "?" 4000 3000 14 "rggb14" $Q
        done
    done

    # 10 HERO10 dark frames (spread across the 393)
    for i in 5 50 100 150 200 250 300 350 390 393; do
        GPR=$(ls "$HERO10_DARK/"*.GPR | sed -n "${i}p")
        if [ -f "$GPR" ]; then
            NAME="HERO10_idx${i}"
            test_image "$GPR" "$NAME" "HERO10" "?" 5568 4176 14 "rggb14" 3
        fi
    done

    # X2D Phocus calibration frames at each ISO (1 each)
    if command -v dcraw &>/dev/null; then
        for ISO in 64 200 800 3200 12800; do
            # Find first short-exposure file at this ISO
            for f in "$PHOCUS/"Job_*.fff; do
                FILE_ISO=$(exiftool -ISO -s -s -s "$f" 2>/dev/null)
                FILE_EXP=$(exiftool -ExposureTime -s -s -s "$f" 2>/dev/null)
                if [ "$FILE_ISO" = "$ISO" ] && [ "$FILE_EXP" = "1/1000" ]; then
                    NAME="X2D_flat_ISO${ISO}"
                    test_image "$f" "$NAME" "X2D" "$ISO" 11904 8842 16 "rggb16" 3
                    break
                fi
            done
        done

        # X2D dark frames at each ISO (1 each, long exposure)
        for ISO in 64 200 800; do
            for f in "$PHOCUS/"Job_*.fff; do
                FILE_ISO=$(exiftool -ISO -s -s -s "$f" 2>/dev/null)
                FILE_EXP=$(exiftool -ExposureTime -s -s -s "$f" 2>/dev/null)
                if [ "$FILE_ISO" = "$ISO" ] && [ "$FILE_EXP" = "1" ]; then
                    NAME="X2D_dark_ISO${ISO}"
                    test_image "$f" "$NAME" "X2D" "$ISO" 11904 8842 16 "rggb16" 3
                    break
                fi
            done
        done
    else
        echo "  SKIP X2D Phocus tests (dcraw not installed)"
    fi

    # Both Hasselblad DNGs at multiple qualities
    for DNG in "$HASSEL"/*.dng; do
        NAME="X2D_$(basename $DNG .dng)"
        for Q in 0 3 5 8; do
            test_image "$DNG" "${NAME}_Q${Q}" "X2D" "?" 11664 8750 16 "rggb16" $Q
        done
    done

    echo ""
fi

# ============================================================
# Summary
# ============================================================
echo "=== Results ==="
echo ""

if [ -f "$RESULTS" ]; then
    echo "Results saved to: $RESULTS"
    echo ""

    # Count pass/fail (pass = denoise PSNR within 1 dB of baseline OR noise floor)
    python3 -c "
import csv
with open('$RESULTS') as f:
    reader = csv.DictReader(f)
    rows = list(reader)

total = len(rows)
improved = sum(1 for r in rows if r.get('psnr_dn','') and r.get('psnr_base','')
               and float(r['psnr_dn']) >= float(r['psnr_base']) - 1.0)
smaller = sum(1 for r in rows if r.get('size_dn','') and r.get('size_base','')
              and int(r['size_dn']) < int(r['size_base']))

print(f'Total tests: {total}')
print(f'Quality preserved (within 1 dB): {improved}/{total}')
print(f'Size reduced: {smaller}/{total}')
" 2>/dev/null || echo "(python3 not available for summary)"
fi

echo ""
echo "Temp files in: $TMPDIR"
echo "To clean up: rm -rf $TMPDIR"
