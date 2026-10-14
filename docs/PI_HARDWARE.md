# Pi 5 Hardware

Raspberry Pi 5 with 64-bit Raspberry Pi OS is the conservative capture
stand-in. Provide cooling, adequate power, and storage measured under the
actual workload. The recorded setup used an 8 GB Pi 5. Hardware capacity alone
does not establish camera readiness.

## Setup

Build with the portable CMake commands in [Getting Started](GETTING_STARTED.md).
On 64-bit Raspberry Pi OS, install prerequisites and build the tested
production revision from the repository root:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config python3 python3-numpy
git worktree add --detach ../gpr-pi-verified fdb9171
cd ../gpr-pi-verified
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
export GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-artifacts}"
mkdir -p "$GPR_ARTIFACT_ROOT"
```

The validation tools live in the validation checkout, not necessarily in that
production revision. Record both revisions when using those tools against a
production binary. The synthetic `tools/pi5_setup.sh` benchmark and its
historical storage verdict are not current capture-readiness evidence.
For the current branch (rather than that pinned historical revision), add
`-DGPR_BUILD_DIAGNOSTICS=ON` to build the evaluation binaries explicitly.

Choose an artifact directory on the storage device being evaluated through
`GPR_ARTIFACT_ROOT`; do not assume a particular mounted volume or device name.

The current workload is native 4096 x 3072 Bayer FUSED capture and full-frame
1024 x 768 preview from the same `.gvid`. Use the matching codec profile,
pixel format, source provenance, and frame count when comparing runs.
[Ship Decision](SHIP_DECISION.md) defines current approval boundaries.

## Build-isolation replay: October 14, 2026

Production `89a6479` was built camera-only with SDK/still applications off,
then diagnostics explicitly enabled for measurement. Validation `83f30c9`
passed all ten selected codec, container, capture, and preview CTests on Pi 5.
All six isolated build profiles also passed: default, camera, stills, utilities,
codec tests, and preview tests, including the latter two profiles' CTests and
invalid-configuration rejection checks.
The stills-only production build passed metadata roundtrips for all four Bayer
phases at 14 and 16 bits, plus the synthetic RAW-to-GPR regression (80.66 dB).
The fresh GP017601 Bayer replay retained 4096 x 3072 pixels for all 120 encodes;
the GVID bytes matched the prior build exactly.

| Check | Prior build | Current build |
|---|---:|---:|
| Buffered capture wall throughput | 23.77 fps | 23.94 fps |
| Capture including final SSD fsync | 17.60 fps | 17.71 fps |
| Capture peak RSS | 55,280 KiB | 55,296 KiB |

Full-frame 1024 x 768 preview pixels also matched on three output frames.
A separate cached 120-frame replay with RGB file writes disabled took 2.97 s
(40.4 fps), with 22,752 KiB peak RSS and 22.421 ms median decode-plus-ISP time.
This is not display presentation or a diverse-scene quality gate. Capture still
misses the 20 fps floor when final SSD durability is included; no new SD or
Mission 1 firmware approval follows from these results.
Receipt identities and hashes are in [Release evidence](release_evidence.json).

## Previous production replay: October 13, 2026

Production `fdb9171` encoded 120 fresh GP017601 frames at 4096 x 3072,
with output identical to the previous build. Buffered throughput was
23.8818 fps (baseline 23.8236); SSD fsync throughput was 17.6722 fps
(baseline 17.6332); peak RSS was 55,440 KiB. This is file replay, not a sensor
test. The buffered result does not prove durable SD capture at 20 fps, and
the measured SSD fsync result is below that floor. No new preview or camera
closure is inferred. [Release evidence](release_evidence.json) records this
verification separately from the older source revision and receipts; no
artifact hashes were supplied for this summary.

## Historical stand-in receipts

The June 25, 2026 420-frame run recorded zero drops, 24.32 fps capture wall
throughput (25.29 median loop), and 25.85 fps full-frame 1024 x 768 preview
wall throughput (36.23 median decode-plus-target). The selected 1,440-frame
aggregate recorded zero drops, valid `.gvid`, 20.50 fps capture wall throughput
(21.52 median loop), and 24.20 fps preview wall throughput (43.86 median).
Receipt identity: `mission1_camera_closure_run_20260625/current_standin_followup`.
These are historical measurements on their recorded setup, not fresh results
for `fdb9171` or current durable SD proof. A later camera-role attempt stopped
at source audit because no camera sensor was enumerated.

## Storage and timing

Measure sustained writes beyond page-cache buffering, including the chosen
flush policy, frame-size distribution, noisy-content behavior, and thermal
state. Derive required bytes/second from actual encoded bytes and target fps.
A card's advertised speed or class is not a sustained capture receipt.
The historical aggregate includes a Lexar SILVER PLUS write-budget pass;
it must not be generalized to current durable capture performance.

Record whole-run wall throughput separately from per-frame median timing.
Include p95/p99 frame times, drops, memory, temperature, output hashes,
decode validation, and interrupted-tail behavior. RAM-only output and short
cached runs are diagnostics, not storage closure.

For genuine camera tests, use
[Mission 1 Quick Validation](GOPRO_MISSION1_QUICK_VALIDATION.md).
A file replay or simulated DMA source on a Pi remains stand-in evidence even
if it meets the timing target.

Use [Capture API](CAPTURE_API.md) for ownership and flush contracts.
Full-resolution legacy still encode timings are a different, older workload;
see the pinned [Stills Pi 5 Timing](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/STILLS_PI5_TIMING.md).
The consolidated [Pi setup history](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/tools/pi5_setup.md)
and [Video Status history](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/VIDEO_STATUS.md)
remain available. Prior storage estimates and
half-resolution runs are preserved in the
[archive](https://github.com/dcliftreaves/gpr/tree/archive/research-and-integration-2026-09-09),
pinned at [`3d675ef`](https://github.com/dcliftreaves/gpr/tree/3d675ef).
