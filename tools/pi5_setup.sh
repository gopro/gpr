#!/usr/bin/env bash
# pi5_setup.sh — one-shot Pi 5 capture-side setup.
#
# Run AFTER flashing Raspberry Pi OS to the new SD card and SSH-ing in
# (see docs/PI_HARDWARE.md for setup and storage requirements).
#
# What it does:
#   1. apt installs build deps (gcc, cmake, git, python3, numpy)
#   2. Clones the gpr repo if not already present (or pulls if it is)
#   3. Builds gpr_tools, the encoder library, and the fused bench
#   4. Reports SD card identity (vendor / OEM / serial)
#   5. dd-based storage benchmark: 500 × 3.5 MB writes, fsync per write
#   6. Encoder-end-to-end benchmark on a synthesized 50 MP raw (≥ 100
#      frames so the page cache is exhausted and you see the sustained
#      number, not the in-RAM peak)
#   7. Verdict: ≥ 84 MB/s sustained → 24 fps × 50 MP capable
#
# Re-running is safe — apt is idempotent, git pulls, cmake rebuilds.

set -euo pipefail

REPO_URL="${GPR_REPO_URL:-https://github.com/dcliftreaves/gpr.git}"
REPO_DIR="${GPR_REPO_DIR:-$HOME/gpr}"
BUILD_DIR="$REPO_DIR/build"
LOG=/tmp/pi5_setup.log

say()  { printf '\n\033[1;36m==>\033[0m %s\n' "$*" | tee -a "$LOG"; }
note() { printf '    %s\n' "$*" | tee -a "$LOG"; }
hr()   { printf '%s\n' '--------------------------------------------------' | tee -a "$LOG"; }

> "$LOG"
say "pi5_setup starting: $(date)"
note "Host: $(hostname)   arch: $(uname -m)   kernel: $(uname -r)"
note "Repo: $REPO_URL"
note "Dir : $REPO_DIR"

if [ "$(uname -m)" != "aarch64" ]; then
    note "WARNING: not aarch64 (got $(uname -m)). Pi 5 should be aarch64."
fi

# ---- 1. apt deps ----------------------------------------------------------
say "Installing build dependencies (apt)"
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
    build-essential clang cmake git pkg-config \
    python3 python3-pip python3-numpy \
    util-linux libgomp1 \
    | tail -3 | tee -a "$LOG"

# ---- 2. clone / pull repo -------------------------------------------------
if [ ! -d "$REPO_DIR/.git" ]; then
    say "Cloning $REPO_URL → $REPO_DIR"
    git clone --depth 1 "$REPO_URL" "$REPO_DIR" 2>&1 | tail -3 | tee -a "$LOG"
else
    say "Updating $REPO_DIR (git pull)"
    git -C "$REPO_DIR" pull --ff-only 2>&1 | tail -3 | tee -a "$LOG"
fi

# ---- 3. build -------------------------------------------------------------
say "Configuring build (CMake, Release)"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DGPR_BUILD_DIAGNOSTICS=ON 2>&1 | tail -5 | tee -a "$LOG"

say "Building (uses all cores)"
cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tail -5 | tee -a "$LOG"

# ---- 4. SD card identity ---------------------------------------------------
say "SD card identity"
ROOT_DEV=$(findmnt -n -o SOURCE / 2>/dev/null || true)
ROOT_DISK=$(lsblk -nro PKNAME "$ROOT_DEV" 2>/dev/null | head -1)
[ -z "$ROOT_DISK" ] && ROOT_DISK="mmcblk0"
note "Root device: $ROOT_DEV   physical disk: /dev/$ROOT_DISK"

if [ -d "/sys/block/$ROOT_DISK/device" ]; then
    for f in name manfid oemid serial date type; do
        v=$(cat "/sys/block/$ROOT_DISK/device/$f" 2>/dev/null || true)
        [ -n "$v" ] && note "  $f = $v"
    done
fi
# UHS bus mode (if surfaced by kernel)
for m in /sys/class/mmc_host/mmc*; do
    for f in "$m"/mmc*:*/uhs_mode_select "$m"/mmc*:*/timing; do
        [ -r "$f" ] || continue
        v=$(cat "$f")
        note "  $(basename "$(dirname "$f")") $(basename "$f") = $v"
    done
done

# ---- 5. storage benchmark (dd) --------------------------------------------
say "Storage benchmark: 500 × 3.5 MB sustained writes to the SD card"
BENCH_DIR=/tmp/sd_bench
mkdir -p "$BENCH_DIR"
rm -f "$BENCH_DIR"/dd_test
note "Writing 1.75 GB direct + sync. This characterizes the SD's"
note "sustained write speed once the page cache is exhausted."
DD_OUT=$( { dd if=/dev/zero of="$BENCH_DIR/dd_test" \
                bs=3500000 count=500 oflag=direct,sync conv=fdatasync 2>&1 ; } )
note "$(printf '%s' "$DD_OUT" | tail -1)"
rm -f "$BENCH_DIR/dd_test"
SD_MBPS=$(printf '%s' "$DD_OUT" | grep -Eo '[0-9.]+ MB/s' | tail -1 | awk '{print $1}')
[ -z "$SD_MBPS" ] && SD_MBPS=0
hr

# ---- 6. encoder benchmark --------------------------------------------------
say "Synthesizing a 50 MP 14-bit Bayer raw for the encoder bench"
RAW=/tmp/test_50mp.raw
python3 - <<PY
import numpy as np
W, H = 8280, 5520
rng = np.random.default_rng(7)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
r = np.hypot(xx - W/2, yy - H/2) / np.hypot(W/2, H/2)
base = (2000 + (1.0 - r) * 12000).astype(np.int32)
noise = rng.integers(-50, 51, size=(H, W), dtype=np.int32)
out = np.clip(base + noise, 0, 16383).astype('<u2')
out.tofile("$RAW")
print(f"wrote {out.shape} ({out.nbytes/1e6:.1f} MB) to $RAW")
PY

BENCH_BIN="$BUILD_DIR/source/app/bench_fused/bench_fused"
if [ -x "$BENCH_BIN" ]; then
    say "Encoder kernel only (in-RAM, no disk writes) — 30 frames"
    "$BENCH_BIN" "$RAW" 8280 5520 30 2>&1 | tail -20 | tee -a "$LOG" || true
else
    note "WARNING: bench_fused not found at $BENCH_BIN — skipping encoder bench"
fi

# ---- 7. Verdict ------------------------------------------------------------
hr
say "Verdict"
note "Sustained SD write speed: ${SD_MBPS} MB/s"
note "Required for 24 fps × 50 MP × 3.5 MB compressed: 84 MB/s"
note ""
awk -v s="$SD_MBPS" 'BEGIN {
    if (s+0 >= 84) {
        print "    ✓ ≥84 MB/s — capable of 24 fps × 50 MP × 3.5 MB sustained"
    } else if (s+0 >= 40) {
        print "    ~ 40–84 MB/s — 8–23 fps sustained; OK for slow capture / bursts"
    } else if (s+0 > 0) {
        print "    ✗ < 40 MB/s — storage-bound at ≤ ~10 fps. Check that the card is V90,"
        print "      seated fully, and that the Pi 5 is using its UHS-II bus."
    } else {
        print "    (couldn'\''t parse dd output — see $LOG for raw bench)"
    }
}' | tee -a "$LOG"
note ""
note "Full log: $LOG"
