#!/usr/bin/env bash
# test_pi_encoder.sh — Pi 5 encoder throughput regression.
#
# Runs the fused encoder kernel at each canonical resolution and asserts
# achieved fps against a locked baseline. Catches regressions in:
#   - encoder code paths (NEON usage, wavelet level count, parallelization)
#   - kernel/userspace performance (CPU governor, freq, page-cache behavior)
#   - hardware changes (SD card slot UHS-II downgrade, USB SSD swap)
#
# Designed to run ON the Pi 5 itself. From the Mac you'd:
#   scp tools/test/test_pi_encoder.sh "$PI_TARGET":/tmp/
#   ssh "$PI_TARGET" /tmp/test_pi_encoder.sh
# (or use SSH directly:)
#   ssh "$PI_TARGET" 'bash -s' < tools/test/test_pi_encoder.sh
#
# Required setup (one-time, on the Pi):
#   - Repository cloned and built (`cmake -B build && cmake --build build -j`)
#   - A fast write path mounted at /mnt/ssd (the SD slot is UHS-I only at
#     ~71 MB/s; a USB 3.0 SSD on Pi 5 gives 300+ MB/s)
#   - CPU governor `performance` set (handled at runtime by this script)
#   - python3 + numpy (for synthesizing test fixtures)

set -euo pipefail

REPO="${GPR_REPO_DIR:-$HOME/gpr}"
BENCH="${BENCH:-$REPO/build/source/app/bench_fused/bench_fused}"
FIXTURE_DIR="${FIXTURE_DIR:-/mnt/ssd}"
NFRAMES="${NFRAMES:-30}"

# Baseline fps thresholds — locked from target expectations. The historical
# 50MP_DEC2 run reached 24.93 fps, but the current strict Labs receipt is
# blocked below target; keep this test's threshold at the production target.
# Margin: assert ≥ 0.8 × measured baseline so jitter doesn't trip the test.
# `_FPS` is the locked baseline; `_THR` is the threshold (80% of baseline).
declare -A FPS_BASELINE=(
    [2K]=290       [UHD]=85     [4K]=80     [13MP]=55   [50MP]=16   [50MP_DEC2]=24.00
)
declare -A FPS_THRESHOLD=(
    [2K]=232       [UHD]=68     [4K]=64     [13MP]=44   [50MP]=12   [50MP_DEC2]=24
)

say()  { printf '\n\033[1;36m==>\033[0m %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }

if [ ! -x "$BENCH" ]; then
    echo "ERROR: bench_fused not built at $BENCH" >&2
    echo "  Run: cmake -B $REPO/build -DCMAKE_BUILD_TYPE=Release -DGPR_BUILD_DIAGNOSTICS=ON && cmake --build $REPO/build -j" >&2
    exit 2
fi
mkdir -p "$FIXTURE_DIR"

# Set CPU governor to performance for the duration of the test, restore at exit.
SAVED_GOV=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "")
restore_gov() {
    if [ -n "$SAVED_GOV" ] && [ "$SAVED_GOV" != "performance" ]; then
        for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
            echo "$SAVED_GOV" | sudo tee "$c" >/dev/null 2>&1 || true
        done
    fi
}
trap restore_gov EXIT

if [ "$SAVED_GOV" != "performance" ]; then
    say "Setting CPU governor to performance for the test"
    for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance | sudo tee "$c" >/dev/null 2>&1 || true
    done
fi
note "Governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
note "Freq:     $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) kHz"
note "Temp:     $(vcgencmd measure_temp 2>/dev/null | tr -d "'\"")"

# --- synthesize fixtures (skip if already present + correct size) -------------
say "Ensuring test fixtures present in $FIXTURE_DIR"
declare -A FIXTURES=(
    [2K]="2048 1080"    [UHD]="3840 2160"   [4K]="4096 2160"
    [13MP]="4656 2792"  [50MP]="8280 5520"  [50MP_DEC2]="8280 5520"
)
for label in "${!FIXTURES[@]}"; do
    read -r W H <<< "${FIXTURES[$label]}"
    path="$FIXTURE_DIR/test_${label,,}.raw"
    expected_size=$(( W * H * 2 ))
    actual_size=0
    [ -f "$path" ] && actual_size=$(stat -c %s "$path" 2>/dev/null || echo 0)
    if [ "$actual_size" -ne "$expected_size" ]; then
        note "  synthesizing $label ($W×$H, $((expected_size/1000000)) MB)…"
        python3 - "$W" "$H" "$path" <<'PYEOF'
import sys, numpy as np
W, H, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
r = np.hypot(xx - W/2, yy - H/2) / np.hypot(W/2, H/2)
base = (2000 + (1.0 - r) * 12000).astype(np.int32)
noise = rng.integers(-50, 51, size=(H, W), dtype=np.int32)
np.clip(base + noise, 0, 16383).astype('<u2').tofile(out)
PYEOF
    fi
done

# --- run the matrix ---------------------------------------------------------
say "Encoder kernel timing — $NFRAMES frames per resolution"
note "Threshold = 80% of locked baseline. PASS if median fps ≥ threshold."
echo

printf '  %-6s %4s × %-4s  %12s  %10s  %10s  %s\n' \
    "Res" "W" "H" "median ms" "fps" "threshold" "verdict"
printf '  %s\n' "---------------------------------------------------------------------"

FAILS=0
for label in 2K UHD 4K 13MP 50MP 50MP_DEC2; do
    read -r W H <<< "${FIXTURES[$label]}"
    path="$FIXTURE_DIR/test_${label,,}.raw"
    if [ "$label" = "50MP_DEC2" ]; then
        times=$(GPR_COL_DECIMATE=2 GPR_ROW_DECIMATE=2 "$BENCH" "$path" "$W" "$H" "$NFRAMES" 2>&1 | grep -E '^[0-9]+\.[0-9]+$' | sort -n)
    else
        times=$("$BENCH" "$path" "$W" "$H" "$NFRAMES" 2>&1 | grep -E '^[0-9]+\.[0-9]+$' | sort -n)
    fi
    n=$(printf '%s\n' "$times" | wc -l | tr -d ' ')
    if [ "$n" -lt 10 ]; then
        printf '  %-6s %4d × %-4d  %12s  %10s  %10s  %s\n' \
            "$label" "$W" "$H" "(no output)" "—" "—" "FAIL (encoder error)"
        FAILS=$((FAILS+1)); continue
    fi
    median=$(printf '%s\n' "$times" | awk -v n="$n" 'NR == int(n/2)+1 {print; exit}')
    fps=$(awk -v m="$median" 'BEGIN {printf "%.1f", 1000/m}')
    thr=${FPS_THRESHOLD[$label]}
    pass=$(awk -v f="$fps" -v t="$thr" 'BEGIN {print (f >= t) ? "PASS" : "FAIL"}')
    [ "$pass" = "FAIL" ] && FAILS=$((FAILS+1))
    printf '  %-6s %4d × %-4d  %12s  %8s fps  %8s fps  %s\n' \
        "$label" "$W" "$H" "$median" "$fps" "$thr" "$pass"
done

echo
note "Final temp: $(vcgencmd measure_temp 2>/dev/null | tr -d "'\"")"
echo
if [ "$FAILS" -eq 0 ]; then
    say "All $((${#FIXTURES[@]})) resolutions PASS"
    exit 0
else
    say "$FAILS resolution(s) FAIL"
    exit 1
fi
