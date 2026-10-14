#!/usr/bin/env bash
# Shared CI/local suite. Optional omissions are reported separately, never PASS.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"
MODE=full
export FAST="${FAST:-0}"
while [ $# -gt 0 ]; do
    case "$1" in
        --native-only) MODE=native ;;
        --fast) export FAST=1 ;;
        -h|--help)
            echo "Usage: $0 [--native-only] [--fast]"
            echo "Build first. BUILD_DIR defaults to build; TMPDIR selects scratch storage."
            echo "Native flags: CC, GPR_TEST_CFLAGS, GPR_TEST_LDFLAGS."
            echo "Python: set PYTHON_BIN; full mode checks codec/CNN/visual metric dependencies."
            echo "Logs: GPR_TEST_ARTIFACT_DIR (or ARTIFACT_DIR); scratch is removed on exit."
            echo "Optional models/media/hardware are reported separately from passes."
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
export PYTHON_BIN="${PYTHON_BIN:-${PY:-python3}}"
PYTHON_BIN="$(command -v "$PYTHON_BIN")"
export PY="$PYTHON_BIN"
export PATH="$(dirname "$PYTHON_BIN"):$PATH"
python3() { "$PYTHON_BIN" "$@"; }
export -f python3
if [ "$MODE" = full ]; then
    "$PYTHON_BIN" -c 'import numpy, rawpy, PIL, torch, torchvision, cv2, tifffile, skimage, pytorch_msssim, lpips'
else
    "$PYTHON_BIN" -c 'import numpy'
fi
export BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
[[ "$BUILD_DIR" = /* ]] || BUILD_DIR="$REPO_ROOT/$BUILD_DIR"
export GTOOLS="${GTOOLS:-$BUILD_DIR/source/app/gpr_tools/gpr_tools}"
export GPR_TOOLS="${GPR_TOOLS:-$GTOOLS}"
export BENCH="${BENCH:-$BUILD_DIR/source/app/bench_fused/bench_fused}"
export GPR2PRORES="${GPR2PRORES:-$REPO_ROOT/tools/gpr2prores/gpr2prores}"
export GPR_EXTERNAL_ROOT="${GPR_EXTERNAL_ROOT:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work}"
LOG_ROOT="${GPR_TEST_ARTIFACT_DIR:-${ARTIFACT_DIR:-$GPR_EXTERNAL_ROOT/artifacts/regressions}}"
mkdir -p "$LOG_ROOT"
LOG_ROOT="$(cd "$LOG_ROOT" && pwd)"
LOG_DIR="$(mktemp -d "$LOG_ROOT/run.XXXXXX")"
mkdir -p "${TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
SCRATCH="$(mktemp -d "${TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}/gpr-regressions.XXXXXX")"
cleanup() {
    local status=$?
    trap - EXIT
    if [ "${GPR_COVERAGE_DIR:-}" != "" ]; then
        mkdir -p "$GPR_COVERAGE_DIR/native-regressions"
        find "$SCRATCH/bin" -type f \( -name '*.gcno' -o -name '*.gcda' \) \
            -exec cp {} "$GPR_COVERAGE_DIR/native-regressions/" \; || status=1
    fi
    rm -rf -- "$SCRATCH" || status=1
    printf 'Exit status: %s\nLogs: %s\n' "$status" "$LOG_DIR" >> "$LOG_DIR/summary.log"
    echo "Scratch removed; logs: $LOG_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
export TMPDIR="$SCRATCH" GPR_TMPDIR="$SCRATCH"
export ARTIFACT_DIR="$SCRATCH/capabilities"
mkdir -p "$SCRATCH/bin"
passed=0 failed=0 skipped=0 partial=0 index=0
skip() { echo "SKIP: $*"; skipped=$((skipped + 1)); }
run() {
    local kind="$1" label="$2" rc=0 log
    shift 2
    index=$((index + 1))
    log="$LOG_DIR/$index.log"
    echo "RUN: $label"
    "$@" 2>&1 | tee "$log" || rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: $label (exit $rc)"; failed=$((failed + 1))
        if [ "$rc" -eq 139 ] && [ "$(uname -s)" = Darwin ] && command -v lldb >/dev/null; then
            lldb --batch -o run -k 'thread backtrace all' -- "$@" \
                > "$LOG_DIR/$index-crash.log" 2>&1 || true
        fi
    elif grep -Eq '(^|[[:space:]:|])SKIP(PED)?([[:space:]:|(]|$)|skipped=[1-9]|[1-9][0-9]* SKIPPED' "$log"; then
        if [ "$kind" = required ]; then
            echo "FAIL: $label skipped required coverage"; failed=$((failed + 1))
        else
            echo "PARTIAL: $label (see log for executed and skipped cells)"
            partial=$((partial + 1))
        fi
    else
        echo "PASS: $label"; passed=$((passed + 1))
    fi
}
# Preserve the structured native fixtures and both noise levels from CI.
python3 - <<'PY' 2>&1 | tee "$LOG_DIR/fixtures.log"
import os
from pathlib import Path
import numpy as np
w, h = 8280, 5520
yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
radius = np.hypot(xx - w/2, yy - h/2) / np.hypot(w/2, h/2)
base = (2000 + (1.0 - radius) * 12000).astype(np.int32)
for name, seed, amplitude in (("Z8_ISO64.raw", 42, 50), ("Z8_ISO22800.raw", 4242, 400)):
    noise = np.random.default_rng(seed).integers(-amplitude, amplitude + 1, size=(h, w), dtype=np.int32)
    path = Path(os.environ["TMPDIR"]) / name
    np.clip(base + noise, 0, 16383).astype("<u2").tofile(path)
    assert path.stat().st_size == w * h * 2
np.clip(base[:512, :512], 0, 16383).astype("<u2").tofile(Path(os.environ["TMPDIR"]) / "preview.raw")
PY
read -r -a cflags <<< "${GPR_TEST_CFLAGS:--O2 -g}"
read -r -a ldflags <<< "${GPR_TEST_LDFLAGS:--lm}"
native=(test_video_format test_labs_encoder_api test_edge_sizes
        test_video_encoder_abort test_video_full_chain test_video_roundtrip
        test_video_pipeline_sim_portable)
for name in "${native[@]}"; do
    "${CC:-clang}" "${cflags[@]}" "source/app/$name.c" \
        "$BUILD_DIR/source/lib/vc5_encoder/libvc5_encoder.a" \
        "$BUILD_DIR/source/lib/vc5_common/libvc5_common.a" \
        "${ldflags[@]}" -lpthread -lm -o "$SCRATCH/bin/$name" 2>&1 | tee "$LOG_DIR/build-$name.log"
done
"${CC:-clang}" "${cflags[@]}" -DPASS2_POOL_FORCE=1 \
    -I source/lib/vc5_common -I source/lib/common/private -I source/lib/common/public \
    source/app/test_fused_pool_drop_hp.c source/lib/vc5_encoder/fused_encode.c \
    "$BUILD_DIR/source/lib/vc5_encoder/libvc5_encoder.a" \
    "$BUILD_DIR/source/lib/vc5_common/libvc5_common.a" \
    "${ldflags[@]}" -lpthread -lm -o "$SCRATCH/bin/test_fused_pool_drop_hp" \
    2>&1 | tee "$LOG_DIR/build-test_fused_pool_drop_hp.log"
run required "forced encoder pool with omitted bands" "$SCRATCH/bin/test_fused_pool_drop_hp"
for name in test_video_format test_labs_encoder_api test_video_encoder_abort test_video_full_chain; do
    run required "$name" "$SCRATCH/bin/$name"
done
for inline in 0 1; do
    run required "edge sizes inline=$inline" env FUSED_INLINE_TOKENIZE="$inline" "$SCRATCH/bin/test_edge_sizes"
done
run required "inline tail flush" "$BUILD_DIR/bin/test_jans_inline_tail_flush"
run required "public Bayer roundtrip" env GPR_INCLUDE_LL=1 FUSED_INLINE_TOKENIZE=0 \
    "$BUILD_DIR/bin/test_fused_roundtrip" "$SCRATCH/Z8_ISO64.raw" 8280 5520
run required "preview GVID encode" env GPR_INCLUDE_LL=1 FUSED_MULTI_LEVEL=0 \
    GPR_ROW_DECIMATE=1 GPR_COL_DECIMATE=1 GPR_BENCH_PIXEL_FORMAT=4 FUSED_QUALITY=3 \
    GPR_BENCH_GVID="$SCRATCH/preview.gvid" GPR_BENCH_GVID_FPS=24 \
    "$BENCH" "$SCRATCH/preview.raw" 512 512 2
run required "native RGB preview" "$BUILD_DIR/bin/gvid_preview_rgb_cli" \
    "$SCRATCH/preview.gvid" 512 512 "$SCRATCH/preview" 2 1
run required "preview output frames" python3 - <<'PY'
import os
from pathlib import Path
frames = sorted((Path(os.environ["TMPDIR"]) / "preview/frames_rgb").glob("*.rgb"))
assert len(frames) == 2, f"expected two RGB frames, got {len(frames)}"
for frame in frames:
    pixels = frame.read_bytes()
    assert len(pixels) == 128 * 128 * 3, frame
    assert len(set(pixels)) > 1, f"blank preview: {frame}"
PY
for iso in ISO64 ISO22800; do
    run required "band roundtrip $iso" env FUSED_INLINE_TOKENIZE=0 FUSED_MULTI_LEVEL=0 \
        GPR_ROW_DECIMATE=1 GPR_COL_DECIMATE=1 GPR_DROP_HIGHPASS=0 \
        "$SCRATCH/bin/test_video_roundtrip" "$SCRATCH/Z8_$iso.raw" 8280 5520 4 3 10 24.0 150.0
done
run required "video pipeline simulator" "$SCRATCH/bin/test_video_pipeline_sim_portable" \
    "$SCRATCH/Z8_ISO64.raw" 8280 5520 4 3 10 24.0 150.0 0.0 1.0
if [ "$MODE" = full ]; then
    run required "sensitive content" python3 tools/test/check_sensitive_content.py
    run required "matrix failure and cleanup harness" python3 tools/test/test_still_matrix_harness.py
    run required "Bayer detail sidecar" python3 tools/test/test_pack_bayer_detail_residual_sidecar.py
    run required "noise black level" python3 tests/quality_gates/test_noise_profile_blacklevel.py
    run required "still quality corpus" bash source/app/test_still_quality_corpus.sh
    run required "still quality matrix" bash tools/test/test_still_matrix.sh
    run mixed "capabilities (optional CNN cells)" env FAST="${GPR_CAPABILITIES_FAST:-$FAST}" \
        "$PYTHON_BIN" tools/test/test_capabilities.py
    run required "quality tables" python3 tools/test/check_fused_quality_tables.py
    run required "GVID conformance" python3 tools/test/test_gvid_conformance.py "$SCRATCH/gvid-conformance"
    for name in \
        test_raw_resolution_targets test_bayer_resample \
        test_extract_raw_bayer_u16 test_build_camera_noise_calibration \
        test_convert_darkframe_calibration_to_noise_sidecars test_build_camera_noise_runtime_policy \
        test_verify_production_artifacts test_mission1_camera_source_probe \
        test_mission1_camera_target_preflight test_mission1_dma_source_sim \
        test_run_mission1_camera_closure test_mission1_camera_closure_run \
        test_package_mission1_sr_sequence_receipt
    do
        run required "$name" python3 "tools/test/$name.py"
    done
    run required "live preview policy" python3 tools/live_preview_policy.py
    for name in \
        test_gpr_tools_raw_gpr_roundtrip test_gvid_pack test_gvid_metadata \
        test_fused_context_env_capture test_bench_fused_quality_env test_bench_fused_stream_source \
        test_labs_encoder_bench_cli test_labs_bundle_builder test_labs_bundle_verify \
        test_labs_target_bench_smoke test_labs_camera_handoff_receipt \
        test_labs_preview_ui_receipt test_build_labs_preview_ui_receipt \
        test_mission1_native12_fll2_t2_profile test_render_gvid_sr_registry
    do
        run required "$name" bash "tools/test/$name.sh"
    done
    for name in test_real_fixture_compatibility test_iphone_dng_input_guard test_mission1_metadata_repack; do
        run optional "$name (local media)" bash "tools/test/$name.sh"
    done
    if [ "$(uname -s)" = Darwin ] && [ -x "$GPR2PRORES" ]; then
        run required "GVID metadata fixture" "$GTOOLS" -i "$SCRATCH/preview.raw" \
            -w 512 -h 512 -x rggb16 -o "$SCRATCH/preview-meta.dng"
        run required "GVID to ProRes" "$GPR2PRORES" \
            --meta-dng "$SCRATCH/preview-meta.dng" --no-cnn \
            --demosaic core-image --out-resolution 2k \
            "$SCRATCH/preview.gvid" "$SCRATCH/gvid-preview.mov"
        run required "GVID ProRes codec and frame count" python3 - "$SCRATCH/gvid-preview.mov" <<'PY'
import json
import os
import subprocess
import sys
probe = subprocess.check_output([
    os.environ.get("FFPROBE", "ffprobe"), "-v", "error", "-count_frames",
    "-select_streams", "v:0", "-show_entries", "stream=codec_name,nb_read_frames",
    "-of", "json", sys.argv[1],
], text=True)
streams = json.loads(probe)["streams"]
assert len(streams) == 1, streams
assert streams[0]["codec_name"] == "prores", streams
assert int(streams[0]["nb_read_frames"]) == 2, streams
print("PASS: two decoded ProRes frames from synthetic GVID")
PY
        run required "renderer DNG fixture" "$GTOOLS" -i "$SCRATCH/Z8_ISO64.raw" \
            -w 8280 -h 5520 -x rggb14 -o "$SCRATCH/render.dng"
        for demosaic in metal-bilinear core-image; do
            run required "ProRes render $demosaic" "$GPR2PRORES" --no-cnn \
                --demosaic "$demosaic" --out-resolution 2k --max-frames 1 \
                "$SCRATCH/render.dng" "$SCRATCH/$demosaic.mov"
        done
        run optional "CNN video matrix" bash tools/test/test_video_pipeline.sh
        run optional "sustained playback (model/hardware)" bash tools/test/test_sustained_playback.sh
        run optional "GVID renderer input (local media)" bash tools/test/test_gpr2prores_gvid_input.sh
    else
        skip "Metal/CNN playback requires macOS, renderer binaries, and optional models/media"
    fi
    if [ "${GPR_RUN_TARGET_BENCH:-0}" = 1 ]; then
        run required "target encoder performance" bash tools/test/test_pi_encoder.sh
    else
        skip "target encoder performance requires GPR_RUN_TARGET_BENCH=1 on target hardware"
    fi
fi
[ "$FAST" = 0 ] || skip "large matrix cells omitted by FAST=1"
echo "Results: $passed passed, $failed failed, $skipped skipped, $partial partial; logs: $LOG_DIR" | tee "$LOG_DIR/summary.log"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    echo "Regression suite: $passed passed, $failed failed, $skipped skipped, $partial partial. Skips/partial runs are not quality passes." >> "$GITHUB_STEP_SUMMARY"
fi
[ "$failed" -eq 0 ]
