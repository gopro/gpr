#!/usr/bin/env bash
# Smoke-test Labs target bench receipt generation without target hardware.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
if [ -z "${GPR_EXTERNAL_ROOT:-}" ]; then
  GPR_EXTERNAL_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work"
fi
GPR_TMPDIR="${GPR_TMPDIR:-$GPR_EXTERNAL_ROOT/tmp}"
# A caller-provided WORK is a parent directory, never a cleanup target.
WORK_ROOT="${WORK:-$GPR_TMPDIR}"
mkdir -p -- "$WORK_ROOT"
WORK=$(mktemp -d "$WORK_ROOT/labs_target_bench_smoke.XXXXXX")
PYTHON_BIN="${PYTHON_BIN:-python3}"

cleanup() {
  local status=$?
  if [ "$status" -eq 0 ] && [ "${GPR_KEEP_TEST_ARTIFACTS:-0}" != "1" ]; then
    rm -rf -- "$WORK"
  fi
}
trap cleanup EXIT

mkdir -p "$WORK/source_snapshot/source/lib" "$WORK/source_snapshot/tools"
cat > "$WORK/source_snapshot/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.16)
project(gpr_source_snapshot_smoke C)
EOF
cat > "$WORK/source_snapshot/source/lib/smoke.c" <<'EOF'
int gpr_source_snapshot_smoke(void) { return 7; }
EOF
cat > "$WORK/source_snapshot/tools/smoke.py" <<'EOF'
#!/usr/bin/env python3
print("source snapshot smoke")
EOF

"$PYTHON_BIN" "$REPO/tools/run_labs_target_bench.py" \
  --simulate \
  --frames 8 \
  --output-dir "$WORK" \
  --capture-width 640 \
  --capture-height 360 \
  --target-fps 24 \
  --source-provenance-root "$WORK/source_snapshot"

"$PYTHON_BIN" - "$WORK/labs_target_bench.json" <<'PY'
import json
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text())
assert receipt["schema"] == "gpr_labs_target_bench.v1"
assert receipt["simulated"] is True
assert receipt["source_provenance"]["available"] is True
assert receipt["source_provenance"]["policy"] == "source_tree_digest_v1"
assert len(receipt["source_provenance"]["sha256"]) == 64
assert receipt["source_provenance"]["file_count"] == 3
assert receipt["source_provenance"]["git"]["available"] is False
assert receipt["capture"]["frames_requested"] == 8
assert receipt["capture"]["frames_written"] == 8
assert receipt["capture"]["dropped_frames"] == 0
assert receipt["gvid"]["validation"]["frame_count"] == 8
assert receipt["interruption_recovery"]["validator_rejects_truncated"] is True
assert receipt["interruption_recovery"]["complete_frames_recovered"] == 7
assert receipt["fused_timing"]["available"] is False
assert receipt["fused_timing"]["timing_line_count"] == 0
assert receipt["writer_handoff"]["wall_includes_writer_drain"] is True
assert receipt["writer_handoff"]["deferred_writer_work_present"] is False
assert receipt["writer_handoff"]["loop_fps_median"] is None
assert receipt["writer_handoff"]["loop_median_ms"] is None
assert receipt["writer_handoff"]["wall_ms_per_frame"] > 0
assert receipt["writer_handoff"]["target_frame_ms"] == 1000.0 / 24.0
assert receipt["writer_handoff"]["wall_target_gap_ms"] < 0
assert receipt["writer_handoff"]["bottleneck_target_gap_ms"] == receipt["writer_handoff"]["wall_target_gap_ms"]
assert "bench_child_maxrss_kb" in receipt["memory"]
assert "loadavg_start" in receipt["cpu"]
assert receipt["storage"]["target"]["name"] == "Lexar Professional SILVER PLUS SDXC/microSDXC UHS-I (128GB-1TB)"
assert receipt["storage"]["target"]["target_read_MBps"] == 205.0
assert receipt["storage"]["target"]["target_write_MBps"] == 150.0
assert "64GB microSD" in receipt["storage"]["target"]["profile_note"]
assert receipt["storage"]["target"]["budget_read_MBps"] == 184.5
assert receipt["storage"]["target"]["fits_target"] is True
assert receipt["verdict"]["gvid_valid"] is True
assert receipt["verdict"]["storage_target_met"] is True
assert receipt["verdict"]["target_evidence"] is False
PY

cat > "$WORK/fake_bench.py" <<'PY'
#!/usr/bin/env python3
import os
import struct
import sys
from pathlib import Path

raw, w, h, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
assert Path(raw).is_file()
out = os.environ.get("GPR_BENCH_GVID")
assert out
fps = float(os.environ.get("GPR_BENCH_GVID_FPS", "24"))
fps_x1000 = int(round(fps * 1000.0))
with open(out, "wb") as f:
    f.write(struct.pack("<IBBHHHIIIII", 0x44495647, 1, 0, 4, 3, 0, w, h, fps_x1000, 0, n))
    for idx in range(n):
        payload = (f"direct-frame-{idx}\n".encode("ascii") * 3)
        f.write(struct.pack("<IIQ", 0x004D5246, len(payload), idx))
        f.write(payload)
        print(f"{1.0 + idx * 0.01:.2f}")
print("# bench_phase_ms encode n=4 mean=0.600 stddev=0.010 min=0.590 p25=0.595 median=0.600 p75=0.605 p95=0.610 p99=0.610 max=0.610", file=sys.stderr)
print("# bench_phase_ms write n=4 mean=0.400 stddev=0.010 min=0.390 p25=0.395 median=0.400 p75=0.405 p95=0.410 p99=0.410 max=0.410", file=sys.stderr)
print("# bench_phase_ms total n=4 mean=1.000 stddev=0.010 min=0.990 p25=0.995 median=1.000 p75=1.005 p95=1.010 p99=1.010 max=1.010", file=sys.stderr)
print("# bench_phase_ms async_drain n=1 mean=50.000 stddev=0.000 min=50.000 p25=50.000 median=50.000 p75=50.000 p95=50.000 p99=50.000 max=50.000", file=sys.stderr)
PY
chmod +x "$WORK/fake_bench.py"
printf '\0%.0s' {1..128} > "$WORK/fake.raw"

FUSED_LOG_POLYNOMIAL=1 \
GPR_DECIMATE_AA=1 \
GPR_BENCH_PIXEL_FORMAT=4 \
GPR_BENCH_GVID_PINGPONG=1 \
GPR_BENCH_GVID_COALESCE_PREFIX=1 \
"$PYTHON_BIN" "$REPO/tools/run_labs_target_bench.py" \
  --bench "$WORK/fake_bench.py" \
  --raw "$WORK/fake.raw" \
  --frames 4 \
  --output-dir "$WORK/direct" \
  --source-width 8 \
  --source-height 8 \
  --capture-width 8 \
  --capture-height 8 \
  --target-fps 1 \
  --direct-gvid \
  --source-provenance-root "$WORK/source_snapshot"

"$PYTHON_BIN" - "$WORK/direct/labs_target_bench.json" <<'PY'
import json
import platform
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text())
expected_target_evidence = (
    platform.system().lower() == "linux"
    and platform.machine().lower() in {"aarch64", "arm64", "armv7l", "armv8l"}
)
assert receipt["simulated"] is False
assert receipt["source_provenance"]["available"] is True
assert receipt["source_provenance"]["file_count"] == 3
assert receipt["capture"]["frames_requested"] == 4
assert receipt["capture"]["frames_written"] == 4
assert receipt["gvid"]["validation"]["frame_count"] == 4
assert receipt["gvid"]["validation"]["fps_x1000"] == 1000
assert receipt["storage"]["fsync_policy"] == "bench_fused sequential .gvid fwrite"
assert receipt["storage"]["target"]["required_write_MBps"] < receipt["storage"]["target"]["budget_write_MBps"]
assert receipt["bench"]["env_overrides"]["GPR_INCLUDE_LL"] == "1"
assert receipt["bench"]["env_overrides"]["FUSED_MULTI_LEVEL"] == "1"
assert receipt["bench"]["env_overrides"]["FUSED_WAVELET_LEVELS"] == "2"
assert receipt["bench"]["env_overrides"]["GPR_COL_DECIMATE"] == "2"
assert receipt["bench"]["env_overrides"]["GPR_ROW_DECIMATE"] == "2"
assert receipt["bench"]["env_overrides"]["FUSED_QUALITY"] == "3"
assert receipt["bench"]["env_overrides"]["FUSED_LOG_POLYNOMIAL"] == "1"
assert receipt["bench"]["env_overrides"]["GPR_DECIMATE_AA"] == "1"
assert receipt["bench"]["env_overrides"]["GPR_BENCH_PIXEL_FORMAT"] == "4"
assert receipt["bench"]["env_overrides"]["GPR_BENCH_GVID_FPS"] == "1.000000"
assert receipt["verdict"]["gvid_valid"] is True
assert receipt["gvid"]["validation"]["fps_x1000"] == 1000
assert receipt["verdict"]["storage_target_met"] is True
assert receipt["verdict"]["target_evidence"] is expected_target_evidence
assert receipt["target"]["target_evidence_forced"] is False
assert receipt["target"]["actual_wall_fps"] > 0
assert receipt["verdict"]["fps_median_target_met"] is True
assert receipt["verdict"]["fps_wall_target_met"] is True
assert receipt["verdict"]["fps_target_met"] is True
assert receipt["bench_phase_timing"]["available"] is True
assert receipt["bench_phase_timing"]["phase_ms"]["async_drain"]["median_ms"] == 50.0
assert receipt["bench_phase_timing"]["dominant_phase_by_mean_ms"] == "encode"
assert receipt["writer_handoff"]["deferred_writer_work_present"] is True
assert receipt["writer_handoff"]["deferred_writer_phase_names"] == ["async_drain"]
assert receipt["writer_handoff"]["deferred_writer_drain_ms"] == 50.0
assert receipt["writer_handoff"]["loop_fps_median"] == 1000.0
assert receipt["writer_handoff"]["loop_median_ms"] == 1.0
assert receipt["writer_handoff"]["wall_fps"] == receipt["target"]["actual_wall_fps"]
assert receipt["writer_handoff"]["wall_ms_per_frame"] > 0
assert receipt["writer_handoff"]["target_frame_ms"] == 1000.0
assert receipt["writer_handoff"]["loop_target_gap_ms"] == -999.0
assert receipt["writer_handoff"]["wall_target_gap_ms"] < 0
assert receipt["writer_handoff"]["bottleneck_target_gap_ms"] < 0
assert receipt["writer_handoff"]["fps_target_met"] is True
PY

set +e
GPR_BENCH_GVID_SCATTER=1 \
"$PYTHON_BIN" "$REPO/tools/run_labs_target_bench.py" \
  --bench "$WORK/fake_bench.py" \
  --raw "$WORK/fake.raw" \
  --frames 4 \
  --output-dir "$WORK/direct_scatter_multilevel_reject" \
  --source-width 8 \
  --source-height 8 \
  --capture-width 8 \
  --capture-height 8 \
  --target-fps 1 \
  --direct-gvid \
  --source-provenance-root "$WORK/source_snapshot" >"$WORK/direct_scatter_multilevel_reject.out" 2>"$WORK/direct_scatter_multilevel_reject.err"
rc=$?
set -e
test "$rc" -ne 0
grep -q "GPR_BENCH_GVID_SCATTER direct .gvid output is only supported with --wavelet-levels 1" \
  "$WORK/direct_scatter_multilevel_reject.err"
test ! -f "$WORK/direct_scatter_multilevel_reject/labs_target_bench.json"

GPR_BENCH_GVID_PINGPONG=1 \
GPR_BENCH_GVID_COALESCE_PREFIX=1 \
"$PYTHON_BIN" "$REPO/tools/run_labs_target_bench.py" \
  --bench "$WORK/fake_bench.py" \
  --raw "$WORK/fake.raw" \
  --frames 4 \
  --output-dir "$WORK/direct_native12" \
  --source-width 8 \
  --source-height 8 \
  --capture-width 8 \
  --capture-height 8 \
  --target-fps 1 \
  --quality 0 \
  --wavelet-levels 1 \
  --no-decimate \
  --direct-gvid \
  --source-provenance-root "$WORK/source_snapshot"

"$PYTHON_BIN" - "$WORK/direct_native12/labs_target_bench.json" <<'PY'
import json
import sys
from pathlib import Path

receipt = json.loads(Path(sys.argv[1]).read_text())
env = receipt["bench"]["env_overrides"]
assert env["FUSED_WAVELET_LEVELS"] == "1"
assert env["FUSED_QUALITY"] == "0"
assert env["GPR_BENCH_GVID_PINGPONG"] == "1"
assert env["GPR_BENCH_GVID_COALESCE_PREFIX"] == "1"
assert "GPR_COL_DECIMATE" not in env
assert "GPR_ROW_DECIMATE" not in env
assert receipt["source_provenance"]["available"] is True
assert receipt["source_provenance"]["file_count"] == 3
assert receipt["capture"]["source_width"] == 8
assert receipt["capture"]["capture_width"] == 8
assert receipt["verdict"]["gvid_valid"] is True
assert receipt["verdict"]["storage_target_met"] is True
assert receipt["verdict"]["fps_median_target_met"] is True
assert receipt["verdict"]["fps_wall_target_met"] is True
assert receipt["verdict"]["fps_target_met"] is True
assert receipt["writer_handoff"]["deferred_writer_work_present"] is True
assert receipt["writer_handoff"]["loop_fps_median"] == 1000.0
assert receipt["writer_handoff"]["loop_median_ms"] == 1.0
assert receipt["writer_handoff"]["target_frame_ms"] == 1000.0
assert receipt["writer_handoff"]["bottleneck_target_gap_ms"] < 0
PY

"$PYTHON_BIN" - "$REPO/tools/run_labs_target_bench.py" <<'PY'
import importlib.util
import sys
from pathlib import Path

mod_path = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("run_labs_target_bench", mod_path)
mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(mod)

sample = """
    ch0: unpack=5.0, horiz=1.0, vert+quant=2.0, tokenize=3.0, other=0.5, TOTAL=11.5
    ch1: wait=0.2, horiz=1.3, vert+quant=2.1, tokenize=3.2, other=0.7, TOTAL=7.5
  FUSED ML Pass1 (level-1, parallel):       20.0ms
  FUSED ML Level23 (streamed in pass1):     0.1ms
  FUSED pred_ll ch0: 4.750ms size=830045
  FUSED pred_ll ch1: 5.500ms size=660872
  FUSED ML Pass2 (40 bands, parallel):      6.0ms
  FUSED ML Total:                           27.0ms
# jans_inline_profile label=ch0_b1 coeffs=786432 zero=332111 nonzero=454321 tokens=454496 resid_bits=894294 stripe_rows=384 defer=1 bytes=297324
# jans_inline_profile label=ch0_b1 coeffs=786432 zero=332100 nonzero=454332 tokens=454507 resid_bits=894301 stripe_rows=384 defer=1 bytes=297331
"""
parsed = mod.parse_fused_timing_stderr(sample)
assert parsed["available"] is True
assert parsed["timing_line_count"] == 8
assert parsed["dominant_stage_by_mean_ms"] == "ml_total"
assert parsed["dominant_channel_component_by_mean_ms"] == "total"
assert parsed["stage_ms"]["ml_pass1"]["mean_ms"] == 20.0
assert parsed["channel_component_ms"]["unpack"]["mean_ms"] == 5.0
assert parsed["channel_component_ms"]["vert_quant"]["n"] == 2
assert parsed["channel_component_by_channel_ms"]["1"]["wait"]["mean_ms"] == 0.2
assert parsed["pred_ll_ms"]["0"]["mean_ms"] == 4.75
assert parsed["pred_ll_size_bytes"]["1"]["mean_ms"] == 660872.0

profile = mod.parse_jans_inline_profile_stderr(sample)
assert profile["available"] is True
assert profile["profile_line_count"] == 2
assert profile["by_label"]["ch0_b1"]["coeffs"]["mean"] == 786432.0
assert profile["by_label"]["ch0_b1"]["tokens"]["n"] == 2
assert profile["by_label"]["ch0_b1"]["tokens"]["mean"] == 454501.5

phase = mod.parse_bench_phase_timing_stderr("""
# bench_phase_ms encode n=4 mean=1.000 stddev=0.000 min=1.000 p25=1.000 median=1.000 p75=1.000 p95=1.000 p99=1.000 max=1.000
# bench_phase_ms total n=4 mean=1.000 stddev=0.000 min=1.000 p25=1.000 median=1.000 p75=1.000 p95=1.000 p99=1.000 max=1.000
# bench_phase_ms pingpong_drain n=1 mean=25.000 stddev=0.000 min=25.000 p25=25.000 median=25.000 p75=25.000 p95=25.000 p99=25.000 max=25.000
""")
handoff = mod.writer_handoff_receipt(phase, frames_written=4, wall_s=0.2, target_fps=24)
assert handoff["deferred_writer_phase_names"] == ["pingpong_drain"]
assert handoff["deferred_writer_drain_ms"] == 25.0
assert handoff["wall_includes_writer_drain"] is True
assert handoff["wall_fps"] == 20.0
assert handoff["loop_median_ms"] == 1.0
assert handoff["wall_ms_per_frame"] == 50.0
assert handoff["target_frame_ms"] == 1000.0 / 24.0
assert handoff["loop_target_gap_ms"] < 0
assert handoff["wall_target_gap_ms"] > 0
assert handoff["bottleneck_target_gap_ms"] == handoff["wall_target_gap_ms"]
assert handoff["fps_target_met"] is False
PY

"$PYTHON_BIN" "$REPO/tools/run_labs_perf_sweep.py" \
  --bench "$WORK/fake_bench.py" \
  --raw "$WORK/fake.raw" \
  --frames 4 \
  --output-dir "$WORK/sweep" \
  --source-width 8 \
  --source-height 8 \
  --capture-width 8 \
  --capture-height 8 \
  --target-fps 2000 \
  --direct-gvid \
  --variant baseline \
  --variant stripe64_defer:FUSED_STRIPE_ROWS=64,FUSED_DEFER_RANS=1

"$PYTHON_BIN" - "$WORK/sweep/labs_perf_sweep.json" <<'PY'
import json
import sys
from pathlib import Path

sweep = json.loads(Path(sys.argv[1]).read_text())
assert sweep["schema"] == "gpr_labs_perf_sweep.v1"
assert sweep["direct_gvid"] is True
assert sweep["simulated"] is False
assert sweep["production_claim"] is False
assert sweep["frames_per_variant"] == 4
assert sweep["target_fps"] == 2000
assert sweep["storage_target"]["target_read_MBps"] == 205.0
assert sweep["storage_target"]["target_write_MBps"] == 150.0
assert sweep["storage_target"]["budget_read_MBps"] == 184.5
assert sweep["storage_target"]["budget_write_MBps"] == 135.0
assert set(sweep["ranked_by_fps_median"]) == {"baseline", "stripe64_defer"}
assert len(sweep["variants"]) == 2
by_name = {item["name"]: item for item in sweep["variants"]}
assert by_name["baseline"]["bench_exit_code"] == 1
assert by_name["baseline"]["completed"] is True
assert by_name["baseline"]["fps_target_met"] is False
assert by_name["baseline"]["storage_target_met"] is True
assert by_name["baseline"]["gvid_valid"] is True
assert by_name["stripe64_defer"]["env"]["FUSED_STRIPE_ROWS"] == "64"
assert by_name["stripe64_defer"]["env"]["FUSED_DEFER_RANS"] == "1"
assert Path(by_name["stripe64_defer"]["receipt"]).is_file()
PY

echo "test_labs_target_bench_smoke: PASS"
