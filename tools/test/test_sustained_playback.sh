#!/usr/bin/env bash
# tools/test/test_sustained_playback.sh
#
# Sustained-fps regression test for UHD playback.
#
# Builds a 24-frame .mov fixture from synthesized 50 MP Z8-shaped DNGs,
# plays it through gpr2prores twice (with BIBO_1x CNN and again with
# --no-cnn), parses `effective fps=` out of the DONE line, and asserts
# both runs clear the configured thresholds (24 fps by default).
#
# Skips cleanly on:
#   - Linux (gpr2prores depends on Metal/MPS/AVFoundation)
#   - missing BIBO_1x metal weights dir ($GPR_ARTIFACT_ROOT/weights/F_ane_1x_weights_metal)
#   - missing gpr2prores / bench_fused / gpr_mov_tool binaries
#
# Env knobs:
#   BUILD_DIR=build             cmake build root
#   WORK=...                    parent for a unique scratch directory
#   GPR_KEEP_TEST_ARTIFACTS=1    retain scratch; otherwise removed on exit
#   FPS_WITH_CNN_MIN=24         production lower bound for CNN run
#   FPS_NO_CNN_MIN=24           production lower bound for no-CNN run
#   FRAMES=24                   number of frames in the fixture
#   PY=...                      python interpreter (must have rawpy)

set -euo pipefail

# ---- platform / dep gate ----------------------------------------------------

if [ "$(uname -s)" != "Darwin" ]; then
    echo "SKIP — test_sustained_playback.sh is macOS-only (gpr2prores depends on Metal/MPS)"
    exit 0
fi

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
    GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-$GPR_EXTERNAL_ROOT/artifacts}"
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
TMPDIR="$GPR_TMPDIR"
BUILD_DIR="${BUILD_DIR:-build}"
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$REPO/$BUILD_DIR"

GTOOLS="${GTOOLS:-$BUILD_DIR/source/app/gpr_tools/gpr_tools}"
BENCH="${BENCH:-$BUILD_DIR/source/app/bench_fused/bench_fused}"
GPR2PRORES="${GPR2PRORES:-$REPO/tools/gpr2prores/gpr2prores}"
MOV="${MOV:-$REPO/tools/gpr2prores/gpr_mov_tool}"
PY="${PY:-$(command -v python3 || true)}"

CKPT_W1X="${CKPT_W1X:-$GPR_ARTIFACT_ROOT/weights/F_ane_1x_weights_metal}"

for t in "$GTOOLS" "$BENCH" "$GPR2PRORES" "$MOV"; do
    if [ ! -x "$t" ]; then
        echo "SKIP — required binary missing: $t"
        echo "       (build via cmake $BUILD_DIR + cd tools/gpr2prores && make)"
        exit 0
    fi
done

if [ ! -x "$PY" ]; then
    # Fall back to python3 on PATH (rawpy may or may not be present)
    PY="$(command -v python3 || true)"
    if [ -z "$PY" ]; then
        echo "SKIP — no python3 available for fixture synthesis"
        exit 0
    fi
fi

if ! "$PY" -c "import rawpy" 2>/dev/null; then
    echo "SKIP — python rawpy module not available (needed to extract bayer from synthesized DNGs)"
    exit 0
fi

if [ ! -d "$CKPT_W1X" ]; then
    echo "SKIP — BIBO_1x metal weights dir not present at $CKPT_W1X"
    echo "       (extract via tools/gpr2prores/extract_F_ane_weights.py)"
    exit 0
fi

FRAMES="${FRAMES:-24}"
FPS_WITH_CNN_MIN="${FPS_WITH_CNN_MIN:-24}"
FPS_NO_CNN_MIN="${FPS_NO_CNN_MIN:-24}"

# ---- workspace --------------------------------------------------------------

WORK_PARENT="${WORK:-$TMPDIR}"
mkdir -p "$WORK_PARENT" "$TMPDIR"
WORK="$(mktemp -d "$WORK_PARENT/gpr-sustained.XXXXXX")"
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

DNG_DIR="$WORK/dngs"
mkdir -p "$DNG_DIR"

echo "==== test_sustained_playback: $(date) ===="
echo "repo            : $REPO"
echo "build_dir       : $BUILD_DIR"
echo "gpr2prores      : $GPR2PRORES"
echo "BIBO_1x weights : $CKPT_W1X"
echo "frames          : $FRAMES"
echo "thresholds      : ≥${FPS_WITH_CNN_MIN} fps (CNN), ≥${FPS_NO_CNN_MIN} fps (no-CNN)"
echo "workspace       : $WORK"
echo

# ---- 1. Synthesize $FRAMES of 50 MP Z8-shaped rggb14 DNGs -----------------
# Same radial-gradient pattern as test_video_pipeline.sh /
# test_still_quality_corpus.sh, but each frame gets a different rng seed
# so the codec rANS streams aren't byte-identical (this avoids any
# accidental short-circuit in caches or dedup).
echo "==> synthesizing $FRAMES test DNGs (8280×5520 rggb14)"
W=8280; H=5520; PEAK=16383

for i in $(seq 0 $((FRAMES - 1))); do
    RAW="$WORK/frame_$(printf '%04d' $i).raw"
    DNG="$DNG_DIR/frame_$(printf '%04d' $i).dng"
    seed=$((2026 + i))
    "$PY" - "$W" "$H" "$PEAK" "$seed" "$RAW" <<'PY'
import sys, numpy as np
W, H, peak, seed, out = (int(sys.argv[1]), int(sys.argv[2]),
                         int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])
rng = np.random.default_rng(seed)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
# slight per-frame center jitter keeps content varying without changing
# overall entropy
cx = W / 2 + (seed % 17) - 8
cy = H / 2 + (seed % 13) - 6
r = np.hypot(xx - cx, yy - cy) / np.hypot(W / 2, H / 2)
bright = (peak * (1.0 - np.minimum(r, 1.0))).astype(np.int32)
img = np.zeros((H, W), dtype=np.int32)
img[0::2, 0::2] = bright[0::2, 0::2] + 200
img[0::2, 1::2] = bright[0::2, 1::2] + 800
img[1::2, 0::2] = bright[1::2, 0::2] + 800
img[1::2, 1::2] = bright[1::2, 1::2] + 400
img += rng.integers(-64, 65, size=(H, W), dtype=np.int32)
np.clip(img, 0, peak, out=img)
img.astype('<u2').tofile(out)
PY
    "$GTOOLS" -i "$RAW" -w "$W" -h "$H" -x rggb14 -o "$DNG" >/dev/null 2>&1 \
        || { echo "ERROR: failed to synth $DNG" >&2; exit 2; }
    rm -f "$RAW"
done

DNG_COUNT=$(ls "$DNG_DIR"/*.dng 2>/dev/null | wc -l | tr -d ' ')
echo "  ${DNG_COUNT} DNGs ready in $DNG_DIR"
[ "$DNG_COUNT" -eq "$FRAMES" ] || { echo "ERROR: expected $FRAMES DNGs got $DNG_COUNT" >&2; exit 1; }

# ---- 2. Build the .mov fixture via the production recipe ----------------
# Re-use the production recipe with a unique child scratch directory.
echo
echo "==> building ${FRAMES}-frame .mov fixture via make_gvid_mov_fixture.sh"
FIXTURE_MOV="$WORK/fixture.mov"
FIX_WORK="$WORK/fixture_scratch"
mkdir -p "$FIX_WORK"
WORK="$FIX_WORK" FRAMES="$FRAMES" FPS=24 \
    BUILD_DIR="$BUILD_DIR" GTOOLS="$GTOOLS" BENCH="$BENCH" MOV="$MOV" PY="$PY" \
    bash "$REPO/tools/test/make_gvid_mov_fixture.sh" "$DNG_DIR" "$FIXTURE_MOV" \
    > "$WORK/fixture_build.log" 2>&1 \
    || { echo "ERROR: fixture build failed; tail of log:" >&2
         tail -n 30 "$WORK/fixture_build.log" >&2; exit 2; }
FIXTURE_SIZE=$(du -h "$FIXTURE_MOV" 2>/dev/null | awk '{print $1}')
echo "  fixture: $FIXTURE_MOV ($FIXTURE_SIZE)"

META_DNG=$(ls "$DNG_DIR"/*.dng | head -1)

# ---- 3. Play through gpr2prores: with CNN, then without -------------------
parse_fps() {
    local log="$1"
    awk '/effective fps=/ {
        n=split($0,a,"effective fps=")
        split(a[2],b," ")
        print b[1]
    }' "$log"
}

run_playback() {
    local label="$1"; shift
    local out_mov="$WORK/${label}_out.mov"
    local log="$WORK/${label}.log"
    # All human-readable output goes to stderr so the only thing this
    # function writes to stdout is the parsed fps value.
    echo                  >&2
    echo "==> playback: $label" >&2
    set +e
    "$GPR2PRORES" "$@" --timing --meta-dng "$META_DNG" \
        "$FIXTURE_MOV" "$out_mov" > "$log" 2>&1
    rc=$?
    set -e
    if [ $rc -ne 0 ]; then
        echo "ERROR: gpr2prores rc=$rc for $label; tail of log:" >&2
        tail -n 30 "$log" >&2
        return 2
    fi
    local fps
    fps=$(parse_fps "$log")
    if [ -z "$fps" ]; then
        echo "ERROR: could not parse effective fps from $log" >&2
        tail -n 10 "$log" >&2
        return 2
    fi
    echo "  $label  fps=$fps" >&2
    printf '%s' "$fps"
}

FPS_CNN=$(run_playback with_cnn \
    --cnn-backend mpsgraph --ckpt "$CKPT_W1X" \
    --cnn-scale 1x --demosaic metal-bilinear --out-resolution uhd)

FPS_NOCNN=$(run_playback no_cnn \
    --no-cnn --demosaic metal-bilinear --out-resolution uhd)

# ---- 4. Assertions --------------------------------------------------------

fails=0
ge() {
    # ge $a $b → true if $a >= $b (floating point)
    awk -v a="$1" -v b="$2" 'BEGIN { exit !(a+0 >= b+0) }'
}

echo
echo "==== assertions ===="
if ge "$FPS_CNN" "$FPS_WITH_CNN_MIN"; then
    printf '  PASS  with-CNN UHD     measured=%s fps  threshold=≥%s fps\n' \
        "$FPS_CNN" "$FPS_WITH_CNN_MIN"
else
    printf '  FAIL  with-CNN UHD     measured=%s fps  threshold=≥%s fps\n' \
        "$FPS_CNN" "$FPS_WITH_CNN_MIN"
    fails=$((fails + 1))
fi

if ge "$FPS_NOCNN" "$FPS_NO_CNN_MIN"; then
    printf '  PASS  no-CNN UHD       measured=%s fps  threshold=≥%s fps\n' \
        "$FPS_NOCNN" "$FPS_NO_CNN_MIN"
else
    printf '  FAIL  no-CNN UHD       measured=%s fps  threshold=≥%s fps\n' \
        "$FPS_NOCNN" "$FPS_NO_CNN_MIN"
    fails=$((fails + 1))
fi

echo
if [ $fails -eq 0 ]; then
    echo "==== sustained-playback OK ===="
    exit 0
else
    echo "==== sustained-playback FAILED ($fails assertion(s)) ===="
    exit 1
fi
