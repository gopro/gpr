#!/usr/bin/env bash
# tools/test/test_still_matrix.sh
#
# Matrix-level still-image regression. Walks ~15 representative
# (resolution × bit-depth × Bayer pattern × quality preset) cells through
# the gpr_tools raw→dng→gpr→dng pipeline and asserts PSNR per cell
# against thresholds locked from a clean master measurement.
#
# This is a strict superset of source/app/test_still_quality_corpus.sh
# along the resolution + quality + bit-depth axes (corpus is 5 rggb14/16
# cases at default quality; this matrix adds rggb12/12p, all normal
# unpacked Bayer phases at 14/16-bit, low & high quality presets, and
# resolutions up through 100 MP X2D).
#
# Fixture pattern: radial gradient + per-channel DC offsets + noise — the
# same synthesizer pattern as test_still_quality_corpus.sh. Specifically
# designed to push 3-level wavelet LL coefficients past 32767 so
# decoder int16/uint16 sign-extension bugs surface as catastrophic PSNR
# drops (caught the Z8 50 MP regression originally).
#
# Per-cell PSNR thresholds were locked from a clean run against
# feature/f_ane-and-width-param HEAD on 2026-05-24:
#   rggb12  q3 1024²              43.28 dB  → threshold 41.0
#   rggb12  q0 1024²              42.95 dB  → threshold 41.0
#   rggb12p q3 1024²              43.27 dB  → threshold 41.0
#   rggb14  q3 1024²              53.76 dB  → threshold 51.5
#   rggb14  q8 1024²              62.06 dB  → threshold 60.0
#   rggb16  q3 1024²              53.44 dB  → threshold 51.0
#   rggb16  q0 1024²              52.84 dB  → threshold 50.5
#   rggb16  q8 1024²              59.63 dB  → threshold 57.5
#   gbrg14  q3 1024²              53.7 dB   → threshold 51.7
#   gbrg16  q3 1024²              53.36 dB  → threshold 51.0
#   grbg14  q3 1024²              53.7 dB   → threshold 51.7
#   bggr14  q3 1024²              53.7 dB   → threshold 51.7
#   grbg16  q3 1024²              53.36 dB  → threshold 51.0
#   bggr16  q3 1024²              53.36 dB  → threshold 51.0
#   rggb12  q3 4032×3024 (12 MP)  43.31 dB  → threshold 41.0
#   rggb14  q8 4032×3024 (12 MP)  62.01 dB  → threshold 60.0
#   rggb14  q3 5568×4176 (23 MP)  53.82 dB  → threshold 51.5
#   rggb14  q3 8280×5520 (Z8)     53.85 dB  → threshold 51.5
#   rggb16  q3 11664×8750 (X2D)   53.52 dB  → threshold 51.0
#   rggb14  q0 8280×5520 (Z8)     53.11 dB  → threshold 50.5
#
# Tolerance ≈ 2 dB below the clean reading, well above legitimate-encoder-
# tweak noise but tight enough to catch sign-extension / wavelet-shape
# regressions that produce 5-15 dB drops.
#
# Pure Python + numpy + rawpy. No committed media. Linux & macOS.
#
# Env knobs:
#   BUILD_DIR=build               (cmake build root)
#   GTOOLS=/path/to/gpr_tools     (override binary path entirely)
#   WORK_DIR=$GPR_TMPDIR          (parent for unique, owned scratch directories)
#   PYTHON_BIN=python3            (interpreter with numpy and rawpy)
#   GPR_KEEP_TEST_ARTIFACTS=1     (preserve scratch for inspection)
#   FAST=1                        (skip ≥23 MP cells for quick CI)
#   MATRIX_TOLERANCE_DB=2.0       (override per-cell tolerance)

set -euo pipefail

BUILD_DIR="${BUILD_DIR:-build}"
GTOOLS="${GTOOLS:-$BUILD_DIR/source/app/gpr_tools/gpr_tools}"
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
    GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
WORK_PARENT="${WORK_DIR:-$GPR_TMPDIR}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
TOL="${MATRIX_TOLERANCE_DB:-2.0}"
FAST="${FAST:-0}"

if [ ! -x "$GTOOLS" ]; then
    echo "ERROR: gpr_tools not at $GTOOLS (set BUILD_DIR or GTOOLS env var)" >&2
    exit 2
fi

if ! "$PYTHON_BIN" - "$TOL" <<'PY'
import importlib
import math
import sys

missing = []
for name in ("numpy", "rawpy"):
    try:
        importlib.import_module(name)
    except ImportError as exc:
        missing.append(f"{name}: {exc}")
if missing:
    sys.exit(f"ERROR: {sys.executable} requires numpy and rawpy: " + "; ".join(missing))
try:
    tolerance = float(sys.argv[1])
except ValueError:
    sys.exit("ERROR: MATRIX_TOLERANCE_DB must be a finite nonnegative number")
if not math.isfinite(tolerance) or tolerance < 0:
    sys.exit("ERROR: MATRIX_TOLERANCE_DB must be a finite nonnegative number")
PY
then
    exit 2
fi

mkdir -p "$WORK_PARENT"
WORK=$(mktemp -d "$WORK_PARENT/gpr-matrix.XXXXXX")
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

# Synthesize a Bayer fixture and roundtrip it through gpr_tools.
# Args: name W H pixel_format peak quality baseline_psnr seed [packed=0]
test_case() (
    local name=$1 W=$2 H=$3 PF=$4 PEAK=$5 Q=$6 BASE=$7 SEED=$8
    local PACKED=${9:-0}
    local run_work="$WORK"
    local WORK
    WORK=$(mktemp -d "$run_work/$name.XXXXXX") || return 1
    trap 'if [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != 1 ]; then rm -rf -- "$WORK"; fi' EXIT
    local raw="$WORK/$name.raw"
    local dng="$WORK/$name.dng"
    local gpr="$WORK/$name.gpr"
    local out="$WORK/${name}_dec.dng"

    # Threshold = baseline_psnr - tolerance, rendered in Python to keep
    # one source of truth for the floating-point arithmetic.
    local THR
    THR=$("$PYTHON_BIN" -c 'import sys; print(float(sys.argv[1]) - float(sys.argv[2]))' "$BASE" "$TOL") || return 1

    # 1. Synthesize: radial gradient + per-channel DC offsets + noise.
    #    Identical pattern shape as test_still_quality_corpus.sh, scaled
    #    by bit depth so the DC offsets are proportionally placed.
    "$PYTHON_BIN" - "$W" "$H" "$PEAK" "$SEED" "$raw" "$PACKED" "$PF" <<'PY' || return 1
import sys, numpy as np
W, H, peak, seed, out_path, packed_flag, pf = sys.argv[1:8]
W, H, peak, seed, packed = int(W), int(H), int(peak), int(seed), int(packed_flag) == 1
rng = np.random.default_rng(seed)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
r = np.hypot(xx - W/2, yy - H/2) / np.hypot(W/2, H/2)
bright = (peak * (1.0 - np.minimum(r, 1.0))).astype(np.int32)
img = np.zeros((H, W), dtype=np.int32)
# DC offsets proportional to peak. Same shape as the corpus script:
# R = ~1/82 of peak, G = ~1/20, B = ~1/41. For rggb14 this matches
# (200 / 16383, 800 / 16383, 400 / 16383) approximately.
off_r = max(1, peak // 82)
off_g = max(1, peak // 20)
off_b = max(1, peak // 41)
if pf.startswith("gbrg"):
    # GBRG: row0 G B G B... row1 R G R G...
    img[0::2, 0::2] = bright[0::2, 0::2] + off_g  # G1
    img[0::2, 1::2] = bright[0::2, 1::2] + off_b  # B
    img[1::2, 0::2] = bright[1::2, 0::2] + off_r  # R
    img[1::2, 1::2] = bright[1::2, 1::2] + off_g  # G2
elif pf.startswith("grbg"):
    # GRBG: row0 G R G R... row1 B G B G...
    img[0::2, 0::2] = bright[0::2, 0::2] + off_g  # G1
    img[0::2, 1::2] = bright[0::2, 1::2] + off_r  # R
    img[1::2, 0::2] = bright[1::2, 0::2] + off_b  # B
    img[1::2, 1::2] = bright[1::2, 1::2] + off_g  # G2
elif pf.startswith("bggr"):
    # BGGR: row0 B G B G... row1 G R G R...
    img[0::2, 0::2] = bright[0::2, 0::2] + off_b  # B
    img[0::2, 1::2] = bright[0::2, 1::2] + off_g  # G1
    img[1::2, 0::2] = bright[1::2, 0::2] + off_g  # G2
    img[1::2, 1::2] = bright[1::2, 1::2] + off_r  # R
else:
    img[0::2, 0::2] = bright[0::2, 0::2] + off_r  # R
    img[0::2, 1::2] = bright[0::2, 1::2] + off_g  # G1
    img[1::2, 0::2] = bright[1::2, 0::2] + off_g  # G2
    img[1::2, 1::2] = bright[1::2, 1::2] + off_b  # B
amp = max(50, peak // 256)
img += rng.integers(-amp, amp + 1, size=(H, W), dtype=np.int32)
np.clip(img, 0, peak, out=img)

if packed:
    # 12-bit packed: every 2 pixels share 3 bytes
    flat = img.astype(np.uint16).ravel()
    if flat.size % 2 != 0:
        raise SystemExit("rggb12p requires even pixel count")
    b = np.empty(flat.size * 3 // 2, dtype=np.uint8)
    b[0::3] = (flat[0::2] & 0xFF).astype(np.uint8)
    b[1::3] = (((flat[0::2] >> 8) & 0x0F) | ((flat[1::2] & 0x0F) << 4)).astype(np.uint8)
    b[2::3] = ((flat[1::2] >> 4) & 0xFF).astype(np.uint8)
    b.tofile(out_path)
else:
    img.astype('<u2').tofile(out_path)
PY

    # 2. raw → dng
    "$GTOOLS" -i "$raw" -w "$W" -h "$H" -x "$PF" -o "$dng" >"$WORK/_log" 2>&1 || {
        printf "  FAIL  %-30s  raw→dng failed\n" "$name"
        tail -n 10 "$WORK/_log" >&2; return 1; }

    # 3. dng → gpr  (quality preset under test)
    "$GTOOLS" -i "$dng" -o "$gpr" -q "$Q" >"$WORK/_log" 2>&1 || {
        printf "  FAIL  %-30s  dng→gpr failed\n" "$name"
        tail -n 10 "$WORK/_log" >&2; return 1; }

    # 4. gpr → dng (production decoder path)
    "$GTOOLS" -i "$gpr" -o "$out" >"$WORK/_log" 2>&1 || {
        printf "  FAIL  %-30s  gpr→dng failed\n" "$name"
        tail -n 10 "$WORK/_log" >&2; return 1; }

    # 5. PSNR check
    "$PYTHON_BIN" - "$dng" "$out" "$PEAK" "$THR" "$BASE" "$name" "$gpr" <<'PY'
import sys, os, numpy as np, rawpy
dng, dec, peak, thr, base, name, gpr = sys.argv[1:8]
peak, thr, base = float(peak), float(thr), float(base)
a = rawpy.imread(dng);  src = a.raw_image.copy().astype(np.float64); a.close()
b = rawpy.imread(dec);  out = b.raw_image.copy().astype(np.float64); b.close()
if src.shape != out.shape:
    print(f"  FAIL  {name:30s}  shape mismatch src={src.shape} dec={out.shape}")
    sys.exit(1)
mse = ((src - out) ** 2).mean()
psnr = 10 * np.log10(peak * peak / mse) if mse > 0 else float("inf")
gpr_kb = os.path.getsize(gpr) / 1024.0
ok = psnr >= thr
status = "PASS" if ok else "FAIL"
print(f"  {status}  {name:30s}  {int(src.shape[0]):>5}x{int(src.shape[1]):<5}  "
      f"GPR={gpr_kb:>8.1f}KB  PSNR={psnr:6.2f} dB  "
      f"(base {base:5.2f}, thr {thr:5.2f})")
sys.exit(0 if ok else 1)
PY
)

echo "==== test_still_matrix: $(date) ===="
echo "Build dir : $BUILD_DIR"
echo "Tolerance : ${TOL} dB below per-cell baseline"
echo "Fast mode : ${FAST}  (1 = skip ≥23 MP cells)"
echo

FAILS=0

# ---- 1024² cells: full bit-depth × quality coverage ----
test_case "rggb12_1024_q3"    1024 1024 rggb12  4095   3  43.28  42 || FAILS=$((FAILS+1))
test_case "rggb12_1024_q0"    1024 1024 rggb12  4095   0  42.95  43 || FAILS=$((FAILS+1))
test_case "rggb12p_1024_q3"   1024 1024 rggb12p 4095   3  43.27  44 1 || FAILS=$((FAILS+1))
test_case "rggb14_1024_q3"    1024 1024 rggb14  16383  3  53.76  45 || FAILS=$((FAILS+1))
test_case "rggb14_1024_q8"    1024 1024 rggb14  16383  8  62.06  46 || FAILS=$((FAILS+1))
test_case "rggb16_1024_q3"    1024 1024 rggb16  65535  3  53.44  47 || FAILS=$((FAILS+1))
test_case "rggb16_1024_q0"    1024 1024 rggb16  65535  0  52.84  48 || FAILS=$((FAILS+1))
test_case "rggb16_1024_q8"    1024 1024 rggb16  65535  8  59.63  49 || FAILS=$((FAILS+1))
test_case "gbrg14_1024_q3"    1024 1024 gbrg14  16383  3  53.70  57 || FAILS=$((FAILS+1))
test_case "gbrg16_1024_q3"    1024 1024 gbrg16  65535  3  53.36  50 || FAILS=$((FAILS+1))
test_case "grbg14_1024_q3"    1024 1024 grbg14  16383  3  53.70  58 || FAILS=$((FAILS+1))
test_case "bggr14_1024_q3"    1024 1024 bggr14  16383  3  53.70  59 || FAILS=$((FAILS+1))
test_case "grbg16_1024_q3"    1024 1024 grbg16  65535  3  53.36  60 || FAILS=$((FAILS+1))
test_case "bggr16_1024_q3"    1024 1024 bggr16  65535  3  53.36  61 || FAILS=$((FAILS+1))

# ---- 12 MP-class cells (4032×3024) ----
test_case "rggb12_12MP_q3"    4032 3024 rggb12  4095   3  43.31  51 || FAILS=$((FAILS+1))
test_case "rggb14_12MP_q8"    4032 3024 rggb14  16383  8  62.01  52 || FAILS=$((FAILS+1))

if [ "$FAST" != "1" ]; then
    # ---- 23 MP HERO10 (5568×4176) ----
    test_case "rggb14_h10_q3"  5568 4176 rggb14  16383  3  53.82  53 || FAILS=$((FAILS+1))

    # ---- 50 MP Z8 (8280×5520) — the original regression target ----
    test_case "rggb14_Z8_q3"   8280 5520 rggb14  16383  3  53.85  54 || FAILS=$((FAILS+1))
    test_case "rggb14_Z8_q0"   8280 5520 rggb14  16383  0  53.11  56 || FAILS=$((FAILS+1))

    # ---- 100 MP X2D (11664×8750) — largest target ----
    test_case "rggb16_X2D_q3" 11664 8750 rggb16  65535  3  53.52  55 || FAILS=$((FAILS+1))
fi

echo
echo "==== $FAILS failure(s) ===="
exit $FAILS
