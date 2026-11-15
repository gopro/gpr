# Stock-compatible 12 MP RAW-video experiment

This experiment records independently decodable **historical VC-5 payloads**
at 4096×3072. It builds on the streaming/NEON/parallel encoder in fork
[PR #99](https://github.com/dcliftreaves/gpr/pull/99); it does not add ANS,
change the quantizers, or require CNN reconstruction to decode a frame.

The executable preloads RAW fixtures and freshly encodes each frame while
cycling through them. This is a storage/codec replay benchmark, not live
sensor capture or a motion-quality test. Input file reading and workspace
first-touch happen before the recording timer. All encodes, payload writes,
pacing and final `fsync` are timed. The four encoder channel workers use all
four Pi CPU cores; preview and CNN are outside capture.

## Build and record

Linux and macOS, using the existing toolchain:

```sh
cmake -S . -B build-video -DCMAKE_BUILD_TYPE=Release \
  -DGPR_BUILD_STOCK_VIDEO_EXPERIMENT=ON
cmake --build build-video --target stock_video --parallel 4
BIN=build-video/source/app/stock_video/stock_video
"$BIN" --input GP011728.raw --input GPAB1094.raw \
  --output clip.gvid --width 4096 --height 3072 \
  --quality 3 --fps 30 --frames 900 --paced > capture.jsonl
python3 tools/stock_video.py inspect clip.gvid > inspect.json
python3 tools/stock_video.py extract clip.gvid --out-dir extracted \
  --decoder "$BIN" --limit 2
```

The inputs are **unpacked 14-bit RGGB in little-endian 16-bit words**, with
no row padding. The app validates the size and sample range. `--input` may
appear up to 16 times; every frame is freshly encoded, not copied from a
precomputed compressed payload. `--frames` controls the replay length.
Quality is the unchanged stock preset 0–5 (default 4). Outputs must not
already exist. All process environment flags needed for the experimental
encoder path are set by the writer.

A reusable 192 MiB arena is first-touched before capture; the tested 12 MP
path used 98,084,352 bytes (~93.5 MiB), in addition to preloaded RAW inputs,
thread stacks and library/runtime memory. Peak arena bytes are not peak RSS.
`--arena-mib 0` selects ordinary per-frame malloc/free. Larger dimensions
require a larger arena; this branch inherits the parent experiment's
allocation-failure limitations.

## What the measurements mean

Each frame emits codec time and service time (encode, output write, release,
plus a frame `fsync` when requested). With `--paced`, deadlines are absolute
frame completion deadlines on a fixed timeline. Late frames are counted and
retained; the writer does not silently drop frames or shift the timeline to
hide backlog. Without pacing, a miss means service time exceeded one target
frame interval.

The final summary separates:

- `buffered_write_fps`: recording loop throughput before the final flush;
- `wall_fps`: throughput including the final `fsync`;
- `service_fps`: sum of service times, excluding pacing sleeps and final flush;
- `final_fsync_s`, latency percentiles, maximum service time and deadline misses;
- `required_MBps`: payload bytes per frame × target FPS, in decimal MB/s;
- workspace capacity, peak callback allocation and setup time.

The first frame and all outliers count. There is no discarded warmup. A
paced clip's last frame can finish before the end of its last frame period;
the loop does not add a cosmetic sleep before reporting throughput. JSONL
logging overhead is included in elapsed throughput, outside per-frame
service time. Ordinary writes may be buffered by the OS; `--fsync-each`
separately tests durable per-frame writes. SSD performance does not establish
microSD or in-camera performance.

Run the quality/rate matrix with an independent parent reference executable:

The benchmark/reference tests use `GPR_BENCH_QUALITY`, which is test-app
instrumentation rather than a historical SDK option. To reproduce the fresh
reference used here, build the unchanged parent **codec library** with this
branch's instrumented single-frame app (no fast-path flags are set by the
reference harness):

```sh
git worktree add --detach /tmp/gpr-video-reference 6b22975
cp source/app/vc5_encoder_app/main.cpp \
  /tmp/gpr-video-reference/source/app/vc5_encoder_app/main.cpp
cmake -S /tmp/gpr-video-reference -B /tmp/gpr-video-reference/build \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gpr-video-reference/build --target vc5_encoder_app --parallel 4
REF=/tmp/gpr-video-reference/build/source/app/vc5_encoder_app/vc5_encoder_app
```

```sh
python3 tools/benchmark_stock_video.py --binary "$BIN" \
  --reference "$REF" \
  --input GP011728.raw --input GPAB1094.raw \
  --out-dir video-results --frames 120 --long-frames 900
```

This covers all six qualities without pacing, Q0/Q3/Q4 paced at 24/30 fps,
Q3 with a per-frame flush, and longer Q3/Q4 30 fps replays. It hashes every
payload against a fresh reference encode of its input at the same quality.
It keeps JSONL receipts and removes large clips after successful verification;
use `--keep-clips` to retain them. Timing and binary/input/output hashes are
preserved. Full Pi results are recorded alongside this document.

## Pi 5 results

Measured on October 27, 2026: Raspberry Pi 5, four Cortex-A76 cores at 2.4 GHz,
performance governor, GCC 12.2/O3/IPO/NEON, USB SSD with ext4. There was no
active throttling at the end observation. All results below include the first
frame and outliers; each run alternates the two supplied real 12 MP RAWs.
[Environment](stock-video-pi5/environment.json),
[summaries and binary/input/output hashes](stock-video-pi5/summary.jsonl),
and per-frame JSONL files in that directory preserve the measurements.

Unpaced 120-frame quality sweep:

| Stock quality | Median encode | Buffered write FPS | FPS including final flush | Payload MB/s at 30 fps |
|---|---:|---:|---:|---:|
| Q0 | 20.07 ms | 47.21 | 43.86 | 33.89 |
| Q1 | 21.81 ms | 40.89 | 34.34 | 49.94 |
| Q2 | 26.33 ms | 34.44 | 27.01 | 86.08 |
| Q3 | 27.41 ms | 32.90 | 25.08 | 101.78 |
| Q4 | 30.44 ms | 29.54 | 22.22 | 120.52 |
| Q5 | 28.31 ms | 30.14 | 21.07 | 154.25 |

The Q4 short unpaced run was slower than Q5 in this single ordered sweep;
these measurements do not establish a monotonic timing relationship or
cross-run variance. Lower presets change quality according to stock behavior.

Longer 900-frame paced runs at a 30 fps target:

| Quality | Median encode | Buffered write FPS | FPS including final flush | Missed completion deadlines | Final flush |
|---|---:|---:|---:|---:|---:|
| Q3 | 26.75 ms | 30.006 | 28.47 | 231/900 | 1.62 s |
| Q4 | 27.50 ms | 30.004 | 28.22 | 294/900 | 1.89 s |

Both average about 30 fps before closing, but service-time tails reach about
132 ms and many frames finish late. Retaining all frames in a replay is not
proof that a live sensor would avoid drops. The serial writer needs bounded
asynchronous storage/capture queues and a tested latency/backpressure budget.
With Q3 and an `fsync` after every frame, the 120-frame test fell to **23.44
fps**, with all 120 completion deadlines missed. Buffered averages alone
would hide this storage/durability difference.

Every payload in all **3,360 frames across 15 runs** matched an independent
fresh parent encoder (`6b22975`) at the same quality. The benchmark does not
establish camera support, unlimited recording, scene-independent 30 fps or
microSD performance. It does show that unchanged stock VC-5 is a practical
RAW-video experiment, with measurable storage and scheduling work remaining.

## Can the existing CNN still be used?

Yes, **after decoding to Bayer, offline**. The container does not determine
whether a Bayer restoration model can run. Matching CFA order, sample
normalization, codec/quality profile and training domain still matters.

`tools/stock_video.py restore` supports the existing `F_ane_no_sr`/`bibo_1x`
checkpoint schema and the fork's newer 1x `CleanupNet` schema, using an
explicit external architecture directory and checkpoint. It preserves the
training wrapper's identity-plus-scaled-residual behavior for BIBO1x,
normalizes by 16383, deinterleaves R/G1/G2/B, applies tiled inference with
context halos, and writes same-resolution 14-bit Bayer in 16-bit words.
Finite output and shape are checked. Original VC-5 and decoded RAW are kept
alongside separate `.cnn.raw` files. The model is loaded once per invocation.

```sh
python tools/stock_video.py restore clip.gvid --out-dir restored \
  --decoder "$BIN" --limit 2 --device mps \
  --cnn-root /path/to/existing/fork/tools/cnn \
  --checkpoint /path/to/BayInBayOut_1x_AAon_w16_ANE_gpr_tools_q3.pt \
  --reference-raw GP011728.raw --reference-raw GPAB1094.raw
```

Only the restore/optional distortion path needs NumPy and PyTorch; the newer
CleanupNet module also imports OpenCV. Use a local model package you trust.
Weights are not bundled or downloaded. CPU/CUDA/MPS device selection is
explicit. Receipts pin checkpoint/architecture hashes. This adapter supports
1x restoration; it does not wire the fused-profile 2x/8K model into this new
container profile or imply that model is matched to stock VC-5.

The tested existing stock-Q3 checkpoint has SHA-256
`df22af432710bddabd223047c2db2d0edf2808dd17c4341694a974e045ec87cd`.
Both full 4096×3072 frames passed inference on Apple M5/MPS. The first took
1.581 s and the second 0.188 s for tiled inference/copy (not decode or file
I/O). These two timings do not establish steady-state CNN throughput.

The quality result was **negative** against the replay reference:

| Input | Stock Q3 PSNR | After CNN PSNR |
|---|---:|---:|
| GP011728 | 62.74 dB | 52.48 dB |
| GPAB1094 | 69.95 dB | 52.11 dB |

The references are RAWs already decoded from supplied GPRs, not original
uncompressed sensor captures. These numbers measure distortion relative to
those references, not sensor-truth quality or perceived improvement. They
do not support enabling this CNN by default. Matching/retraining and held-out
RAW/video, rendered detail/color and temporal-flicker checks are still needed.
See [the complete CNN receipt](stock-video-cnn.json).

## Experimental wire profile

This is **GVID version 2, stock `VC5\0` payload profile**, not the existing
fused-encoder GVID v1. Existing v1 readers must reject it. It is an experiment,
not an allocation of a final public format/version or firmware API. The
dedicated reader verifies every fixed field and rejects unsupported versions.
Changing payload kind rather than disguising stock bytes as fused bytes is
necessary for reliable decoder selection.

All integers are little-endian. The 64-byte header contains:

| Offset | Size | Value |
|---:|---:|---|
| 0 | 4 | `GVID` |
| 4 | 2 | Version = 2 |
| 6 | 2 | Header bytes = 64 |
| 8 | 4 | Payload kind = `VC5\0` |
| 12, 16 | 4 each | Bayer width, height |
| 20, 24 | 4 each | FPS numerator, denominator (integer FPS, denominator 1) |
| 28 | 4 | Stock quality 0–5 |
| 32 | 4 | `RGGB` |
| 36 | 4 | Sample bits = 14 |
| 40 | 4 | Frame header bytes = 32 |
| 44 | 20 | Reserved zero |

Each 32-byte frame header is `FRM2`, u32 header bytes (32), u64 frame index
(starting at zero), u64 nominal PTS in nanoseconds, and u64 payload length.
The payload immediately follows. PTS is generated from the requested rate,
not a sensor timestamp. No interframe prediction, new quantization, or entropy
format is added. Frame payloads can be extracted byte-for-byte as `.vc5`.

The inspector checks framing, lengths, sequence numbers, timestamps and exact
EOF; it rejects truncated clips instead of claiming a complete recording.
It computes payload SHA-256 receipts, but the format itself has no checksum,
final frame count, recovery index, camera calibration or per-frame exposure/
gyro metadata. Cleanly removing complete trailing records is not detectable.
Standalone RAW exports therefore require matching external camera metadata
before meaningful DNG/rendered export. This remains a host experiment.

## Validation and remaining work

```sh
python3 tests/stock_video_test.py --binary "$BIN" \
  --reference /path/to/parent/vc5_encoder_app
```

Tests reject every byte truncation of a minimal clip, wrong profile/fields,
duplicate indices and wrong timestamps. Integration covers all six stock
qualities, two seeded random inputs, repeated four-frame sequences, arena vs
malloc, independent stock payload comparison and RAW decode sizes. The same
suite passes ASan/UBSan with `UBSAN_OPTIONS=halt_on_error=1` (Mac leak detection
is unavailable). This exposed and fixes inherited signed-shift UB in the RAW
decoder and bit-mask helper, plus a 32-bit shift in a debug assertion; payload
comparisons remain exact. Handwritten assembly is not sanitizer-instrumented.

Remaining capture work includes live sensor buffers, asynchronous/bounded
storage queues and backpressure policy, sustained high-detail/noisy/moving
inputs, thermal/power measurements, camera preview and real microSD tests.
The parent architecture branch's failure-path/API/platform limitations still
apply. The independent stock bitstreams preserve stock codec compatibility;
the experimental container needs its dedicated reader.
