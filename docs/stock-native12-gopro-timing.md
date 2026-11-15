# Native 12 MP GoPro replay on Raspberry Pi 5

The unchanged capture executable **fails the combined 20 fps recording/preview
floor on both fixed repeats** using the supplied native-resolution GoPro files.
The earlier FIFO passes used two different, downsampled 50 MP still sources;
they do not establish timing coverage for these inputs. All accepted frames
remain byte-exact against an independently built historical stock encoder,
with exact source calibration and calibrated offline preview pixels.

## Inputs and fixed protocol

`GP011728.GPR` and `GPAB1094.GPR` are SDK-decoded to tightly packed
4096x3072 RGGB14 words, preserving their native geometry and actual typed
calibration. These reference counts have already undergone camera GPR
compression; they are not lossless sensor truth. The raw SHA-256 values are
`bc58754c48a16e232d7bc7de91f680b40cb2766b4350ae641427a0c9b9936d21`
and `17777a9324025519e8c03497d0197beb98eb386ee0a5e447427013e3f3c2b054`.
Their new stock Q3 payloads are 3,632,104 and 3,153,500 bytes respectively.
Neither the prior replay payload references nor its preview CRCs are reused.

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native12-gopro-receipts/protocol.json) is frozen before
the first measured child: two repeats, each 600 attempts at 20 fps, Q3, leased
input, queue four, workspace 192 MiB, 1 MiB writeback and final fsync. Every
accepted frame offers a calibrated 1024x768 memory-CRC preview reconstructed
from encoder bands. Compute stages are serialized and preview is foreground.
The exact command and hashes of binaries, sources, profiles, input metadata,
driver and references are recorded. Independent stock references and offline
preview CRCs are prepared before either repeat.

Capture executable SHA-256 remains
`c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`;
the historical reference encoder remains
`05b1fac320aa772c14cc9812d9715684c997d1c4eb32027baa561edbb828117a`.
Production source, queues, model weights and default scheduling are unchanged.

Only child scheduling is selected through `sudo chrt` before dropping privileges
with `setpriv`. The capture process itself has UID/GID 1000, nice 0, SCHED_FIFO
priority 1 and zero effective capabilities, independently checked at startup.
Global realtime runtime/period remain 950000/1000000 microseconds. No unrelated
Pi build, test or profiler runs during the measured children; no kernel worker,
old benchmark or controller is paused or modified. Payload/preview verification
runs only after each child finishes.

## Combined recording and preview

| Fixed repeat | Recorded / attempted | Overruns | Durable fps | Encode median ms | Render median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|---:|---:|
| A | 600 / 600 | 0 | 19.9961 | 36.8130 | 9.86526 | 32 | 223 |
| B | 570 / 600 | 30 | 18.9284 | 39.8519 | 9.12785 | 570 | 570 |

The [complete receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native12-gopro-receipts/receipt.json) preserves
both failures. All 1,170 accepted payloads and preview images verify exactly;
all source/attempt associations, calibration, queue accounting and END3 markers
verify. This does not convert the 30 dropped attempts into success. Both
`floor.recording` and `floor.preview` are false in both repeats.

Repeat A delivers 300 previews per source: GP011728 has 200 late previews
(maximum 64.2823 ms), GPAB1094 has 23 (maximum 62.3304 ms). Repeat B delivers
275 and 295 previews respectively, all late; their maxima are 213.835 and
214.623 ms. Unequal accepted source counts follow actual attempt overruns and
are explicitly verified. No average or percentile replaces the 50 ms deadline.

The encode and render medians leave little room in a 50 ms period, but their
sum does not reconstruct a critical path or identify the cause of the
repeat-to-repeat difference. These runs do not isolate entropy, cache effects,
storage or scheduler interference.

## Recording-only controls

After both combined runs finish, a separate frozen
[control protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native12-gopro-receipts/recording-control/protocol.json)
removes the preview options and fixes two additional 600-attempt repeats. All
other capture options, native inputs, Q3 reference bytes, scheduling, identity,
writeback and final durability requirements remain the same. Both controls
pass the recording floor:

| Recording-only repeat | Recorded / attempted | Overruns | Durable fps | Encode median ms | Late writes |
|---|---:|---:|---:|---:|---:|
| A | 600 / 600 | 0 | 20.0079 | 24.1228 | 0 |
| B | 600 / 600 | 0 | 20.0073 | 25.2736 | 0 |

All 1,200 control payloads and calibration records match the independent
reference, and END3/accounting verify. The
[control receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native12-gopro-receipts/recording-control/receipt.json)
has `floor.recording=true` for both runs. `floor.preview=false` is expected in
these controls because no preview is requested; it is not a combined pass.

The [event audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native12-gopro-receipts/event-audit.json) independently
recomputes counts, late deliveries and medians from emitted events. Per-source
encode medians in combined A/B are 37.48/42.24 ms for GP011728 and 35.23/39.60 ms
for GPAB1094; without preview they are 25.46/26.74 and 23.78/24.69 ms. The
reported encode interval includes retained-band preview reconstruction inside
the encoder callback when that path is enabled. It is not a measurement of
compression alone. Color rendering is reported separately.

These controls direct the next optimization toward the preview-enabled path:
retained-band reconstruction, color rendering and their compute/memory
interaction with encoding. They do not isolate a kernel cost or prove a sole
cause: recording-only controls follow the combined runs, without an interleaved
order or continuous system tracing. In particular, neither source entropy nor
the stock encoder alone is established as the limiting factor. New proposals
must preserve the exact stock bytes and preview pixels on these sources as
well as the earlier corpus. Simply disabling preview cannot satisfy the
combined requirement.

After all runs, the executable hash is unchanged, the observer is back at
ordinary priority, realtime limits are unchanged and the pre-existing June
processes remain running. A post-run snapshot reports 2.400 GHz, 56.5 C and
no current throttling flags (`0x80000` contains a historical flag). This is not
continuous thermal evidence. All four capture children finished successfully;
functional success and timing-floor failures are reported separately.

## Scope and reproduction

This is file-backed warm-cache replay to the SSD and a memory sink, with setup
and source loading excluded. It does not qualify sensor/DMA, rear display,
camera SD, thermal soak or physical A/V synchronization. No CNN runs in capture.

The archived drivers and launcher use absolute Pi paths and a fresh output
directory. Adapt these deliberately for another machine. Prepare metadata with
`raw_media.extract(source_gpr, 14)` and SDK-decode the named GPRs; compare the
resulting source, raw and metadata hashes with `inputs.json` before running.
Keep the independent historical encoder and preview compiler/player available.
The driver refuses to restart a root that already contains its protocol.

Actual RAWs, source metadata and GVID media remain external under
`/mnt/ssd/gpr_work/stock-native12-gopro-20261030` on Pi and
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_native12_gopro_timing_20261030`
locally. Public receipts contain numeric events, code and hashes, without the
photos or full source metadata. The [earlier scheduling study](stock-process-scheduling.md)
retains its scoped passes and failures separately.

The subsequent [horizontal inverse SIMD experiment](stock-horizontal-inverse.md)
passes its independent oracle and improves the isolated Pi kernel by 9.05%,
but its native-input combined ABBA comparison remains inconsistent and fails
all four strict floors. The proposal is discarded; production stays unchanged.

The [retained-band storage study](stock-preview-retention.md) separately measures
6.7–7.0 ms for retention before inverse reconstruction, then tests spacing and
two scratch-row proposals. Improvements in isolation do not establish a
consistent combined win across controls; all proposals are discarded and the
original executable is restored. New restoration checks pass 27 Mac/24 Pi
Release targets. The native-input timing requirement remains open.
