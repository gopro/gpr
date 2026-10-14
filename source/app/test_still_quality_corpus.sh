#!/usr/bin/env bash
# test_still_quality_corpus.sh
#
# Encode → decode → PSNR roundtrip on synthetic but realistic Bayer
# images covering each canonical (size, bit-depth, Bayer pattern)
# combination the codec is expected to support.
#
# Why this exists: regressions to the encode/decode pair can be silent
# at the existing tests' granularity (test_edge_sizes only verifies
# the encoder returns success; test_video_full_chain only verifies
# container framing; the fused-decode tests only exercise the fused
# path, not the production gpr_sdk path). The Z8 50 MP regression on
# branch feature/encoder-pi5-optimizations went past all of those
# because the wavelet's level-3 LL coefficients depend on the data —
# the random/synthetic fixtures the existing tests use kept LL well
# under 32768 and never tripped the int16/uint16 sign-extension bug
# that real Z8 14-bit photographic data triggers immediately.
#
# This test uses a *radial bright/dark gradient* per-channel-offset
# Bayer pattern — same structure as gpr_tools' own Z8 synthesizer —
# specifically chosen to push 3-level wavelet LL well past 32767
# even at 1024×1024 sizes, so the same bug would fail this test.
#
# Pass criterion: PSNR ≥ THRESHOLD_DB for every case. Tight thresholds
# (~45-55 dB) chosen empirically from a clean master build to allow
# fp16/rounding tolerance but reject anything close to the 14 dB
# Z8 regression.
#
# Requirements (CI): python3 with numpy installed, gpr_tools binary
# at BUILD_DIR/source/app/gpr_tools/gpr_tools (default: build/).

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
GTOOLS="${GTOOLS:-$BUILD_DIR/source/app/gpr_tools/gpr_tools}"
WORK="${WORK_DIR:-${GPR_TMPDIR:-${TMPDIR:-/tmp}}/gpr-still-quality}"
THRESHOLD_DB="${STILL_QUALITY_THRESHOLD_DB:-45.0}"

if [ ! -x "$GTOOLS" ]; then
    echo "ERROR: gpr_tools not at $GTOOLS (set BUILD_DIR or GTOOLS env var)" >&2
    exit 2
fi

mkdir -p "$WORK"
rm -rf "$WORK"/*

# ----------- helpers ----------------

# Args: name w h pixel_format peak threshold seed
test_case() {
    local name=$1 W=$2 H=$3 PF=$4 PEAK=$5 THR=$6 SEED=$7
    local raw="$WORK/$name.raw"
    local dng="$WORK/$name.dng"
    local gpr="$WORK/$name.gpr"
    local out="$WORK/${name}_dec.dng"

    # 1. Synthesize: radial gradient + per-channel DC offsets + noise.
    #    Drives wavelet LL well past 32767 (the regression boundary).
    python3 - "$W" "$H" "$PEAK" "$SEED" "$raw" <<'PY'
import sys, numpy as np
W, H, peak, seed, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
rng = np.random.default_rng(seed)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
r = np.hypot(xx - W/2, yy - H/2) / np.hypot(W/2, H/2)
bright = (peak * (1.0 - np.minimum(r, 1.0))).astype(np.int32)
img = np.zeros((H, W), dtype=np.int32)
# Bayer pattern: R G G B (RGGB) — per-channel DC offsets so each plane
# sits at a different midpoint, exercising sign-extension in each.
img[0::2, 0::2] = bright[0::2, 0::2] + 200  # R
img[0::2, 1::2] = bright[0::2, 1::2] + 800  # G1
img[1::2, 0::2] = bright[1::2, 0::2] + 800  # G2
img[1::2, 1::2] = bright[1::2, 1::2] + 400  # B
amp = max(50, peak // 256)
img += rng.integers(-amp, amp + 1, size=(H, W), dtype=np.int32)
np.clip(img, 0, peak, out=img)
img.astype('<u2').tofile(out)
PY

    # 2. RAW → DNG (so the gpr_tools dng→gpr→dng pipeline can roundtrip).
    "$GTOOLS" -i "$raw" -w "$W" -h "$H" -x "$PF" -o "$dng" >"$WORK/_log" 2>&1 || {
        echo "  FAIL [$name]: raw→dng failed"; cat "$WORK/_log" >&2; return 1; }

    # 3. DNG → GPR (production encoder path)
    "$GTOOLS" -i "$dng" -o "$gpr" >"$WORK/_log" 2>&1 || {
        echo "  FAIL [$name]: dng→gpr failed"; cat "$WORK/_log" >&2; return 1; }

    # 4. GPR → DNG (production decoder path — this is where the bugs hide)
    "$GTOOLS" -i "$gpr" -o "$out" >"$WORK/_log" 2>&1 || {
        echo "  FAIL [$name]: gpr→dng failed"; cat "$WORK/_log" >&2; return 1; }

    # 5. Measure PSNR between source and decoded raw Bayer.
    python3 - "$dng" "$out" "$PEAK" "$THR" "$name" "$gpr" <<'PY'
import sys, numpy as np, rawpy
src_path, dec_path, peak, thr_str, name, gpr_path = sys.argv[1:7]
peak = float(peak); threshold = float(thr_str)
src_raw = rawpy.imread(src_path); src = src_raw.raw_image.copy().astype(np.float64); src_raw.close()
dec_raw = rawpy.imread(dec_path); dec = dec_raw.raw_image.copy().astype(np.float64); dec_raw.close()
if src.shape != dec.shape:
    print(f"  FAIL [{name}]: shape mismatch src={src.shape} dec={dec.shape}")
    sys.exit(1)
mse = ((src - dec) ** 2).mean()
psnr = 10 * np.log10(peak * peak / mse) if mse > 0 else float("inf")
import os
gpr_kb = os.path.getsize(gpr_path) / 1024.0
ok = psnr >= threshold
status = "PASS" if ok else "FAIL"
print(f"  {status}  {name:30s}  {src.shape[0]}x{src.shape[1]:<5}  GPR={gpr_kb:>6.1f}KB  PSNR={psnr:6.2f} dB  (threshold {threshold:.1f})")
sys.exit(0 if ok else 1)
PY
}

echo "==== test_still_quality_corpus: $(date) ===="
echo "Build dir : $BUILD_DIR"
echo "Threshold : $THRESHOLD_DB dB (configurable via STILL_QUALITY_THRESHOLD_DB)"
echo

FAILS=0

# rggb14 cases (Z8-class — the bug class this test catches).
#   Pass thresholds chosen from clean-master measurement: 1024² → 55 dB,
#   bumped down to 45 for margin against legitimate encoder tweaks.
test_case "rggb14_1024"  1024 1024 rggb14 16383 45 42  || FAILS=$((FAILS+1))
test_case "rggb14_2048"  2048 2048 rggb14 16383 45 43  || FAILS=$((FAILS+1))
test_case "rggb14_VGA"    640  480 rggb14 16383 45 44  || FAILS=$((FAILS+1))
test_case "rggb14_UHD"   3840 2160 rggb14 16383 45 45  || FAILS=$((FAILS+1))

# rggb16 cases (X2D-class).
test_case "rggb16_1024"  1024 1024 rggb16 65535 45 50  || FAILS=$((FAILS+1))

echo
echo "==== $FAILS failure(s) ===="
exit $FAILS
