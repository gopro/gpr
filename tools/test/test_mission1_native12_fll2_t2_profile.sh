#!/usr/bin/env bash
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PYTHON_BIN="${PYTHON_BIN:-python3}"
WORK_ROOT="${GPR_TMPDIR:-${TMPDIR:-/tmp}}"
mkdir -p -- "$WORK_ROOT"
WORK=$(mktemp -d "$WORK_ROOT/mission1_fll2_profile_smoke.XXXXXX")

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf -- "$WORK"
  fi
}
trap cleanup EXIT

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" describe > "$WORK/profile.json"

"$PYTHON_BIN" - "$WORK/profile.json" <<'PY'
import json
import sys
from pathlib import Path

profile = json.loads(Path(sys.argv[1]).read_text())
assert profile["profile_id"] == "mission1_native12_fll2_t233_avg7555_fast_pinp2_20fps_v1"
assert profile["bench_args"]["target_fps"] == 20.0
assert profile["bench_args"]["quality"] == 8
assert profile["bench_args"]["wavelet_levels"] == 1
assert profile["bench_args"]["no_decimate"] is True
assert profile["bench_args"]["pixel_format"] == 1
assert profile["bench_args"]["storage_target_read_mbps"] == 205.0
assert profile["bench_args"]["storage_target_write_mbps"] == 150.0
assert profile["bench_args"]["storage_target_safety_margin"] == 0.9
assert "128GB-1TB" in profile["bench_args"]["storage_target_name"]
assert "64GB microSD" in profile["bench_args"]["storage_target_note"]
env = profile["env"]
assert env["FUSED_PIN"] == "1"
assert env["FUSED_PIN_P2"] == "1"
assert env["FUSED_RAW_LL"] == "1"
assert env["FUSED_LL_PREDICT"] == "1"
assert env["FUSED_LL_PREDICTOR"] == "avg"
assert env["FUSED_LL_RICE_KS"] == "7,5,5,5"
assert env["FUSED_LL_RICE_FAST"] == "1"
assert env["FUSED_LL_ASSUME_U16"] == "1"
assert env["GPR_INLINE_DENOISE_HARD"] == "1"
assert env["GPR_INLINE_DENOISE_T_LH"] == "2"
assert env["GPR_INLINE_DENOISE_T_HL"] == "3"
assert env["GPR_INLINE_DENOISE_T_HH"] == "3"
assert env["FUSED_STRIPE_ROWS"] == "384"
PY

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" command \
  --bench /tmp/bench_fused \
  --raw /tmp/GP017602.raw \
  --output-dir /tmp/out \
  --tmpdir /tmp/gpr_tmp \
  --frames 1440 > "$WORK/command.txt"

grep -q 'FUSED_LL_PREDICT=1' "$WORK/command.txt"
grep -q 'FUSED_LL_PREDICTOR=avg' "$WORK/command.txt"
grep -q 'FUSED_LL_RICE_KS=7,5,5,5' "$WORK/command.txt"
grep -q 'FUSED_LL_RICE_FAST=1' "$WORK/command.txt"
grep -q 'FUSED_LL_ASSUME_U16=1' "$WORK/command.txt"
grep -q 'FUSED_PIN_P2=1' "$WORK/command.txt"
grep -q 'GPR_INLINE_DENOISE_T_LH=2' "$WORK/command.txt"
grep -q 'GPR_INLINE_DENOISE_T_HL=3' "$WORK/command.txt"
grep -q 'GPR_INLINE_DENOISE_T_HH=3' "$WORK/command.txt"
grep -q -- '--target-fps 20.0' "$WORK/command.txt"
grep -q -- '--storage-target-write-mbps 150.0' "$WORK/command.txt"
grep -q -- '--storage-target-note' "$WORK/command.txt"
grep -q -- '--direct-gvid' "$WORK/command.txt"

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" command \
  --bench /tmp/bench_fused \
  --raw /tmp/GP017602.raw \
  --output-dir /tmp/out24 \
  --tmpdir /tmp/gpr_tmp \
  --frames 120 \
  --target-fps 24 \
  --cleanup-payloads > "$WORK/command24.txt"

grep -q 'FUSED_LL_RICE_KS=7,5,5,5' "$WORK/command24.txt"
grep -q -- '--frames 120' "$WORK/command24.txt"
grep -q -- '--target-fps 24.0' "$WORK/command24.txt"
grep -q -- '--direct-gvid' "$WORK/command24.txt"
grep -q -- '_gpr_bench_rc=$?' "$WORK/command24.txt"
grep -q -- 'rm -rf /tmp/out24/capture.gvid /tmp/out24/capture_interrupted_tail.gvid /tmp/out24/frames' "$WORK/command24.txt"
grep -q -- 'exit $_gpr_bench_rc' "$WORK/command24.txt"

cp "$WORK/command24.txt" "$WORK/GP017602_120f_24fps_command.sh"
chmod +x "$WORK/GP017602_120f_24fps_command.sh"
"$PYTHON_BIN" - "$WORK" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

work = Path(sys.argv[1])
script = work / "GP017602_120f_24fps_command.sh"
digest = hashlib.sha256(script.read_bytes()).hexdigest()
manifest = {
    "schema": "mission1_pending_pi_probe_manifest.v1",
    "profile_id": "smoke",
    "target": "Pi 5 / Mission 1 stand-in",
    "status": "pending_pi_ssh",
    "raw": "/tmp/GP017602.raw",
    "bench": "/tmp/bench_fused",
    "commands": [
        {
            "file": script.name,
            "frames": 120,
            "target_fps": 24.0,
            "sha256": digest,
            "output_dir": "/tmp/out24",
        }
    ],
    "cleanup_policy": {
        "enabled": True,
        "after_success": ["capture.gvid", "capture_interrupted_tail.gvid", "frames/"],
        "preserve": ["labs_target_bench.json"],
    },
}
(work / "pending_manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" validate-pending \
  --manifest "$WORK/pending_manifest.json"

mkdir -p "$WORK/raws"
touch "$WORK/raws/GP017601.raw" "$WORK/raws/GP017602.raw" "$WORK/raws/GP017603.raw"
cat > "$WORK/fake_roundtrip.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
image=$(basename "$1" .raw)
case "$image" in
  GP017601) bytes=5471604; psnr=84.47; mse=1.0; minv=10; maxv=15770; mean=348.8 ;;
  GP017602) bytes=5571400; psnr=85.14; mse=0.8; minv=4; maxv=15752; mean=268.6 ;;
  GP017603) bytes=5200413; psnr=75.35; mse=7.8; minv=24; maxv=15789; mean=1683.8 ;;
  *) exit 9 ;;
esac
printf 'ENCODE: %s bytes in 1.0 ms\n' "$bytes" >&2
printf 'DECODE: 4096x3072 in 1.0 ms\n' >&2
printf 'PSNR14 (full-res): %s dB  mse=%s\n' "$psnr" "$mse" >&2
printf 'Decoded stats: min=%s  max=%s  mean=%s  npx=12582912\n' "$minv" "$maxv" "$mean" >&2
printf 'decoded' > "$4"
SH
chmod +x "$WORK/fake_roundtrip.sh"

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" validate-local \
  --roundtrip "$WORK/fake_roundtrip.sh" \
  --raw-dir "$WORK/raws" \
  --tmpdir "$WORK" \
  --output-summary "$WORK/local_roundtrip.json"

grep -q '"all_pass": true' "$WORK/local_roundtrip.json"
grep -q '"encoded_bytes": 5571400' "$WORK/local_roundtrip.json"

cat > "$WORK/fake_compare_quality.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
image=$(basename "$1" .raw)
case "$image" in
  GP017601) psnr=84.47; ssim=0.999997; rmse=1.00; maxerr=138 ;;
  GP017602) psnr=85.14; ssim=0.999997; rmse=0.80; maxerr=108 ;;
  GP017603) psnr=75.35; ssim=0.999991; rmse=7.80; maxerr=319 ;;
  *) exit 9 ;;
esac
cat <<EOF
=== Comprehensive Quality Comparison ===
Global Metrics:
  PSNR:           $psnr dB
  SSIM:           $ssim

Error Distribution:
  RMSE:           $rmse DN
  Max error:      $maxerr DN
EOF
SH
chmod +x "$WORK/fake_compare_quality.sh"

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" quality-local \
  --roundtrip "$WORK/fake_roundtrip.sh" \
  --compare-quality "$WORK/fake_compare_quality.sh" \
  --raw-dir "$WORK/raws" \
  --tmpdir "$WORK" \
  --output-summary "$WORK/local_quality.json"

grep -q '"all_pass": true' "$WORK/local_quality.json"
grep -q '"gpr_bytes": 5571400' "$WORK/local_quality.json"
grep -q '"SSIM": 0.999997' "$WORK/local_quality.json"

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" quality-local \
  --roundtrip "$WORK/fake_roundtrip.sh" \
  --compare-quality "$WORK/fake_compare_quality.sh" \
  --raw-dir "$WORK/raws" \
  --tmpdir "$WORK" \
  --stripe-rows 264 \
  --output-summary "$WORK/local_quality_stripe264.json"

grep -q '"FUSED_STRIPE_ROWS": "264"' "$WORK/local_quality_stripe264.json"
grep -q '"all_pass": true' "$WORK/local_quality_stripe264.json"

"$PYTHON_BIN" - "$WORK" <<'PY'
import json
import sys
from pathlib import Path

work = Path(sys.argv[1])
rows = {
    "GP017601": (22.91, 43.64, 109.43208, 5471604),
    "GP017602": (22.21, 45.03, 111.428, 5571400),
    "GP017603": (23.95, 41.75, 104.00826, 5200413),
}
for image, (fps, median, write_mbps, bytes_per_frame) in rows.items():
    receipt = {
        "schema": "gpr_labs_target_bench.v1",
        "target": {"fps": 20.0},
        "verdict": {
            "fps_target_met": True,
            "storage_target_met": True,
            "gvid_valid": True,
            "no_drops": True,
            "interruption_recovery_proven": True,
            "target_evidence": True,
        },
        "timing": {
            "fps_median": fps,
            "median_ms": median,
            "p95_ms": median + 3.0,
            "max_ms": median + 20.0,
        },
        "bench_phase_timing": {
            "phase_ms": {
                "encode": {"median_ms": median - 3.0},
                "write": {"median_ms": 3.0},
            }
        },
        "storage": {
            "gvid_bytes": bytes_per_frame * 1440,
            "target": {
                "required_write_MBps": write_mbps,
                "budget_write_MBps": 135.0,
                "MiB_per_frame": bytes_per_frame / 1048576.0,
            },
        },
    }
    (work / f"{image}_labs_target_bench.json").write_text(json.dumps(receipt), encoding="utf-8")
PY

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" summarize-target \
  --receipt "GP017601=$WORK/GP017601_labs_target_bench.json" \
  --receipt "GP017602=$WORK/GP017602_labs_target_bench.json" \
  --receipt "GP017603=$WORK/GP017603_labs_target_bench.json" \
  --require-all \
  --output "$WORK/target_from_receipts.json"

grep -q '"all_pass": true' "$WORK/target_from_receipts.json"
grep -q '"receipt":' "$WORK/target_from_receipts.json"
grep -q '"target_fps": 20.0' "$WORK/target_from_receipts.json"

"$PYTHON_BIN" - "$WORK/target_from_receipts.json" "$WORK/target_strict24_fail.json" "$WORK/target_results.json" <<'PY'
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
results_dst = Path(sys.argv[3])
summary = json.loads(src.read_text())
summary["target_fps"] = 24.0
dst.write_text(json.dumps(summary), encoding="utf-8")
results_summary = json.loads(src.read_text())
results_summary["results"] = results_summary.pop("rows")
results_summary.pop("all_pass", None)
results_dst.write_text(json.dumps(results_summary), encoding="utf-8")
PY

cat > "$WORK/quality.json" <<'JSON'
{
  "schema": "mission1_native12_current_profile_quality.v1",
  "profile_id": "mission1_native12_fll2_t233_avg7555_fast_pinp2_20fps_v1",
  "passes_20fps_storage_budget_all": true,
  "rows": [
    {"image": "GP017601", "gpr_bytes": 5471604, "required_MBps_at_20fps": 109.43208, "PSNR14_dB": 84.47, "SSIM": 0.999997},
    {"image": "GP017602", "gpr_bytes": 5571400, "required_MBps_at_20fps": 111.428, "PSNR14_dB": 85.14, "SSIM": 0.999997},
    {"image": "GP017603", "gpr_bytes": 5200413, "required_MBps_at_20fps": 104.00826, "PSNR14_dB": 75.35, "SSIM": 0.999991}
  ]
}
JSON

cat > "$WORK/target.json" <<'JSON'
{
  "schema": "mission1_fll2_T233_native12_1440f_20fps_summary.v1",
  "all_pass": true,
  "rows": [
    {"image": "GP017601", "verdict": {"fps_target_met": true, "storage_target_met": true, "gvid_valid": true, "no_drops": true, "interruption_recovery_proven": true, "target_evidence": true}, "fps_median": 22.91, "required_write_MBps": 109.43208, "budget_write_MBps": 135.0},
    {"image": "GP017602", "verdict": {"fps_target_met": true, "storage_target_met": true, "gvid_valid": true, "no_drops": true, "interruption_recovery_proven": true, "target_evidence": true}, "fps_median": 22.21, "required_write_MBps": 111.428, "budget_write_MBps": 135.0},
    {"image": "GP017603", "verdict": {"fps_target_met": true, "storage_target_met": true, "gvid_valid": true, "no_drops": true, "interruption_recovery_proven": true, "target_evidence": true}, "fps_median": 23.95, "required_write_MBps": 104.00826, "budget_write_MBps": 135.0}
  ]
}
JSON

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" validate \
  --quality-summary "$WORK/quality.json" \
  --target-summary "$WORK/target_from_receipts.json"

"$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" validate \
  --quality-summary "$WORK/quality.json" \
  --target-summary "$WORK/target_results.json"

"$PYTHON_BIN" - "$WORK/quality.json" "$WORK/stale_quality.json" <<'PY'
import json
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
summary = json.loads(src.read_text())
summary["schema"] = "mission1_fll2_T2_native12_quality.v1"
summary.pop("profile_id", None)
dst.write_text(json.dumps(summary), encoding="utf-8")
PY

if "$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" validate \
  --quality-summary "$WORK/stale_quality.json" \
  --target-summary "$WORK/target_from_receipts.json" >/dev/null 2>&1; then
  echo "stale quality summary unexpectedly passed" >&2
  exit 1
fi

if "$PYTHON_BIN" "$REPO/tools/mission1_native12_fll2_t2_profile.py" validate \
  --quality-summary "$WORK/quality.json" \
  --target-summary "$WORK/target_strict24_fail.json" >/dev/null 2>&1; then
  echo "strict-24 target summary unexpectedly passed" >&2
  exit 1
fi

echo "test_mission1_native12_fll2_t2_profile: PASS"
