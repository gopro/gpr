#!/usr/bin/env bash
# tools/test/test_video_pipeline.sh
#
# gpr2prores playback-pipeline matrix sweep on a synthesized 50 MP Z8-shaped
# DNG fixture. Walks (CNN variant × output resolution × demosaic backend),
# captures per-stage timing (read / decode / cnn / demosaic / write) and
# the equivalent fps, and asserts every cell completes without error.
#
# This is macOS-only — the gpr2prores binary depends on AVFoundation,
# CoreImage, Metal, and (for CoreML) MPSGraph. Linux skips gracefully.
#
# Timing is reported for inspection; no fps or latency thresholds are enforced.
#
# Required CNN weight directories (produced by extract_F_ane_weights.py /
# extract_F_weights.py in the dering_proto_v2/ tree):
#   $GPR_ARTIFACT_ROOT/weights/F_ane_1x_weights_metal       — F_ane (w=16) 1×
#   $GPR_ARTIFACT_ROOT/weights/F_ane_w32_1x_weights         — F_ane_w32 1×
#   $GPR_ARTIFACT_ROOT/weights/F_legacy_weights_metal       — F_legacy 2× SR
#
# Cells that need missing weight dirs are reported SKIP, not FAIL — this
# keeps the test runnable on a clean checkout, where weight extraction
# hasn't been run yet.
#
# Env knobs:
#   BUILD_DIR=build               (cmake build root; only used to find gpr_tools)
#   GTOOLS=...                    (override gpr_tools path)
#   GPR2PRORES=...                (override gpr2prores path)
#   WORK_DIR=$GPR_TMPDIR/gpr-vidmtx (parent for unique scratch directories)
#   GPR_KEEP_TEST_ARTIFACTS=1      (default; set 0 to remove scratch on exit)
#   FAST=1                        (only test 2k + uhd output sizes)

set -euo pipefail

if [ "$(uname -s)" != "Darwin" ]; then
    echo "SKIP — test_video_pipeline.sh is macOS-only (gpr2prores depends on AVFoundation/Metal)"
    exit 0
fi

BUILD_DIR="${BUILD_DIR:-build}"
GTOOLS="${GTOOLS:-$BUILD_DIR/source/app/gpr_tools/gpr_tools}"
GPR2PRORES="${GPR2PRORES:-tools/gpr2prores/gpr2prores}"
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
    GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-$GPR_EXTERNAL_ROOT/artifacts}"
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
WORK_PARENT="${WORK_DIR:-$GPR_TMPDIR/gpr-vidmtx}"
FAST="${FAST:-0}"

# Resolve to absolutes so the script works regardless of cwd.
if [ ! -x "$GTOOLS" ]; then
    GTOOLS_CANDIDATE="$(pwd)/$GTOOLS"
    [ -x "$GTOOLS_CANDIDATE" ] && GTOOLS="$GTOOLS_CANDIDATE"
fi
if [ ! -x "$GPR2PRORES" ]; then
    GPR_CANDIDATE="$(pwd)/$GPR2PRORES"
    [ -x "$GPR_CANDIDATE" ] && GPR2PRORES="$GPR_CANDIDATE"
fi

if [ ! -x "$GTOOLS" ]; then
    echo "ERROR: gpr_tools not at $GTOOLS (set BUILD_DIR or GTOOLS)" >&2; exit 2; fi
if [ ! -x "$GPR2PRORES" ]; then
    echo "ERROR: gpr2prores not at $GPR2PRORES (set GPR2PRORES, or build with"
    echo "       cd tools/gpr2prores && make gpr2prores)" >&2; exit 2; fi

mkdir -p "$WORK_PARENT"
WORK="$(mktemp -d "$WORK_PARENT/video-matrix.XXXXXX")"
cleanup_run() {
    if [ "${GPR_KEEP_TEST_ARTIFACTS:-1}" = 1 ]; then
        printf 'Scratch retained: %s\n' "$WORK"
    else
        rm -rf -- "$WORK"
    fi
}
trap cleanup_run EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---- 1. Synthesize a 50 MP Z8-shaped DNG via raw→dng using the
#         same gradient pattern as test_still_matrix.sh. -------------------
W=8280; H=5520; PEAK=16383
FIXTURE_RAW="$WORK/Z8_fixture.raw"
FIXTURE_DNG="$WORK/Z8_fixture.dng"

python3 - "$W" "$H" "$PEAK" "$FIXTURE_RAW" <<'PY'
import sys, numpy as np
W, H, peak, out = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
rng = np.random.default_rng(2026)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
r = np.hypot(xx - W/2, yy - H/2) / np.hypot(W/2, H/2)
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

"$GTOOLS" -i "$FIXTURE_RAW" -w "$W" -h "$H" -x rggb14 -o "$FIXTURE_DNG" >"$WORK/_log" 2>&1 \
    || { echo "ERROR: failed to synthesize fixture DNG" >&2; cat "$WORK/_log" >&2; exit 2; }

# ---- 2. Define matrix. ----------------------------------------------------
#
# CNN variants: name | scale | weights_dir
#   (skip cells whose weight dir is missing — friendlier on clean checkouts)
cnn_variants=(
    "F_ane_w16_1x|1x|$GPR_ARTIFACT_ROOT/weights/F_ane_1x_weights_metal"
    "F_ane_w32_1x|1x|$GPR_ARTIFACT_ROOT/weights/F_ane_w32_1x_weights"
    "F_legacy_2x|2x|$GPR_ARTIFACT_ROOT/weights/F_legacy_weights_metal"
)

if [ "$FAST" == "1" ]; then
    resolutions=(2k uhd)
else
    resolutions=(2k uhd 4k 6k 8k)
fi
demosaics=(metal-bilinear core-image)

# ---- 3. Run each cell, capture timing. -----------------------------------
echo "==== test_video_pipeline: $(date) ===="
echo "gpr_tools  : $GTOOLS"
echo "gpr2prores : $GPR2PRORES"
echo "Fixture    : $FIXTURE_DNG (${W}x${H} rggb14)"
echo "Fast mode  : ${FAST}"
echo

# Header for tab-separated summary table at the end.
HEADER=$'cnn\tscale\tres\tdemosaic\tread_ms\tdecode_ms\tcnn_ms\tdemosaic_ms\twrite_ms\ttotal_ms\teff_fps\tstatus'
SUMMARY="$HEADER"

FAILS=0; SKIPS=0; PASSES=0

for vc in "${cnn_variants[@]}"; do
    IFS='|' read -r cnn_name cnn_scale ckpt_dir <<<"$vc"

    if [ ! -d "$ckpt_dir" ]; then
        for res in "${resolutions[@]}"; do
            for dm in "${demosaics[@]}"; do
                SKIPS=$((SKIPS+1))
                SUMMARY+=$'\n'"${cnn_name}	${cnn_scale}	${res}	${dm}	-	-	-	-	-	-	-	SKIP(no-weights)"
            done
        done
        echo "  SKIP  ${cnn_name} (weights dir $ckpt_dir not present)"
        continue
    fi

    for res in "${resolutions[@]}"; do
        for dm in "${demosaics[@]}"; do
            cell="${cnn_name}__${res}__${dm}"
            log="$WORK/${cell}.log"
            mov="$WORK/${cell}.mov"

            set +e
            "$GPR2PRORES" \
                --max-frames 1 --timing \
                --out-resolution "$res" \
                --cnn-scale "$cnn_scale" \
                --cnn-backend metal \
                --ckpt "$ckpt_dir" \
                --demosaic "$dm" \
                "$FIXTURE_DNG" "$mov" >"$log" 2>&1
            rc=$?
            set -e

            if [ $rc -ne 0 ]; then
                FAILS=$((FAILS+1))
                echo "  FAIL  ${cell}  (exit ${rc})"
                tail -n 5 "$log" | sed 's/^/        /' >&2
                SUMMARY+=$'\n'"${cnn_name}	${cnn_scale}	${res}	${dm}	-	-	-	-	-	-	-	FAIL"
                continue
            fi

            # Parse "frame 0  read=Xms decode=Yms cnn=Zms demosaic=Wms write=Vms total=Tms"
            # plus the "effective fps=" line.
            stats=$(awk '
                /frame 0/ {
                    for (i=1;i<=NF;i++) {
                        split($i, kv, "=")
                        if (kv[1] == "read")      r = kv[2]
                        if (kv[1] == "decode")    d = kv[2]
                        if (kv[1] == "cnn")       c = kv[2]
                        if (kv[1] == "demosaic")  m = kv[2]
                        if (kv[1] == "write")     w = kv[2]
                        if (kv[1] == "total")     t = kv[2]
                    }
                }
                /effective fps=/ {
                    n=split($0, a, "effective fps=")
                    split(a[2], b, " ")
                    fps = b[1]
                }
                END {
                    gsub("ms","",r); gsub("ms","",d); gsub("ms","",c)
                    gsub("ms","",m); gsub("ms","",w); gsub("ms","",t)
                    print r"\t"d"\t"c"\t"m"\t"w"\t"t"\t"fps
                }
            ' "$log")

            PASSES=$((PASSES+1))
            printf "  PASS  %-32s  res=%-3s  demosaic=%-13s  %s\n" \
                   "${cnn_name}__${cnn_scale}" "$res" "$dm" "$stats"
            SUMMARY+=$'\n'"${cnn_name}	${cnn_scale}	${res}	${dm}	${stats}	PASS"
        done
    done
done

echo
echo "==== matrix summary (tab-separated) ===="
printf '%s\n' "$SUMMARY"
echo
echo "==== ${PASSES} pass / ${SKIPS} skip / ${FAILS} fail ===="
exit $FAILS
