# Streaming follow-ups: measured controls and retained source

The streaming implementation at `3b2c0f0` passes all seven hosted jobs in
[run 41184771455](https://github.com/dcliftreaves/gpr/actions/runs/41184771455).
That revision corrects the synthetic inverse-row fixture to use valid cubic
symbols; production codec arithmetic remains the measured streaming source.
The experiments below are rejected. A separate
[prepared coefficient lookup](stock-stream-dequantization.md) is adopted after
exact-output checks and a faster matched native Pi comparison. Further
[quantizer-specific tables](stock-stream-quant-tables.md) have a repeatable
processing gain with explicit added storage, while paced deadline failures
remain recorded. None changes production code, defaults,
public APIs, models or the Pi capture executable. The native 12 MP combined
zero-late deadline gate remains open.

## Profile after streaming integration

A separate 120-attempt replay samples user cycles at 199 Hz using the same
native sources, calibrated quarter preview, serialized compute, FIFO1 child,
rolling writeback and final fsync. The profiler runs with privileges while the
capture child runs as UID 1000; no global scheduler/device changes accompany it.
There are approximately 5,000 samples and no lost samples. Startup/setup are
included, and sampling overhead prevents treating this run as timing approval.

| Symbol | Sampled user cycles |
|---|---:|
| Incremental highpass entropy encoder | 35.40% |
| RGGB14 assembly channel unpacker | 24.15% |
| Direct vertical filter | 8.85% |
| Streaming row callback, including inlined consumer work | 6.89% |
| Calibrated wavelet display rows | 4.59% |
| Dequantized inverse row | 4.30% |
| Generic memcpy | 4.00% |
| Horizontal filter | 3.83% |
| ARM CRC | 1.80% |

All 117 accepted recordings/previews out of 120 profiled attempts pass the
existing independent payload, calibration, attempt/PTS, CRC/pixel and END3
verification before temporary clip deletion. The persistent receipt includes
clip bytes and SHA-256. This is a profiled diagnostic with overruns, not a new
sustained result. Annotated entropy instructions are archived; instruction
sample locations do not establish exact instruction costs because of skid.

The writer already combines the existing payload CRC into the stream CRC.
Adding that optimization again would not remove another writer scan. These
samples support concentrating further investigation on encoding/unpacking;
they do not predict the speedup of any particular proposal.

## Progressive calibrated color

A private prefix renderer reuses the existing calibrated row function as each
channel publishes immutable reconstructed rows with release/acquire ordering.
One additional color thread consumes the common ready prefix during encoding.
The control uses the existing four-worker renderer after encoding. Thread
creation/join are included. An actual first color chunk appears around 6 ms,
so the experiment exercises overlap rather than waiting for encoding to finish.

Eight fixed blocks per source use post/progressive/progressive/post/
progressive/post/post/progressive order. Each has sixteen iterations, with
its first iteration excluded: sixty measurements per mode/source.

| Native source | Post-color median ms | Progressive-color median ms |
|---|---:|---:|
| GP011728 | 45.16248 | 56.52610 |
| GPAB1094 | 49.97572 | 53.37932 |

Every progressive block median is slower than every matched post-color block
median on both sources. Encoding itself slows, outweighing the overlap. All
256 iterations preserve stock payload bytes, all independently decoded planes
and calibrated RGB exactly. This rejects the additional-thread configuration;
it does not establish why it slows or reject every possible fused color design.
No capture, pacing, queue, storage or final fsync is timed here.

## Existing shared full-frame unpack control

A second fixed eight-block comparison keeps color after encoding and toggles
only the existing streaming-unpack request inside the harness. Both modes keep
the same remaining fast-path flags and encoder row callback.

| Native source | Channel-streamed median ms | Full-frame unpack median ms |
|---|---:|---:|
| GP011728 | 45.43460 | 72.76888 |
| GPAB1094 | 45.63922 | 69.39064 |

Every full-frame block is slower than every streaming control block. All 256
payload/plane/RGB comparisons remain exact. The full-frame route changes input
preparation/storage as well as sharing work; this does not isolate a newly
designed green cache. It rules out selecting that existing fallback as the
faster configuration for these inputs.

## Short trailing-zero entropy groups

The current fast packer requires the fourth coefficient to be nonzero. The
prototype also batches a shorter prefix through the last nonzero coefficient,
then leaves trailing zeros in the ordinary pending run. It retains canonical
cross-row RLC and the existing <=32-bit join/fallback. This differs from the
previous rejected bulk scanners, pair table and wider reservoir experiments.

The extracted control matches current production function text exactly. On
both Mac and Pi, 356,202 comparisons preserve stream bytes: widths 1..257,
21 coefficient patterns including all sixteen zero/nonzero lane masks, whole
and incremental rows, padded pitches and initial bit occupancies 0..32.

Eight same-binary small-coefficient runs each encode 50,000 streams. Inputs are
generated before timing; the LCG low-bit pattern has period sixteen. Every
candidate run is slower than every control: medians are 0.263077/0.283207 s on
Pi (**7.65% slower**) and 0.054274/0.065585 s on Mac (**20.84% slower**).
This is a synthetic prerequisite failure, so no whole-encoder/capture candidate
is built or promoted. Stream comparisons are against the unchanged control,
not an independent exhaustive mathematical proof of the codec.

## Shared green calculations between unpack workers

A separate four-worker prototype computes GS/GD once per source row and shares
those samples through atomic row-ready publication. The first worker claims
each row; other workers wait for its immutable samples and perform their own
remaining color work. Full-height GS/GD sample buffers use 12,582,912 bytes,
plus readiness metadata. Buffers and status reset are outside timing; worker
creation/join are included. Workers are pinned to cores 0..3, with ordinary
UID 1000 scheduling.

The first readiness array uses adjacent atomic words. A separate layout control
spaces status words 128 bytes apart, addressing potential false sharing without
changing sample math or introducing stricter input alignment.

| Layout / native source | Existing assembly median ms | Shared-green median ms |
|---|---:|---:|
| Adjacent / GP011728 | 12.91986 | 25.46634 |
| Adjacent / GPAB1094 | 12.18064 | 27.68648 |
| Spaced / GP011728 | 12.51742 | 27.67508 |
| Spaced / GPAB1094 | 12.33400 | 27.12336 |

Each layout uses eight fixed blocks, sixteen iterations per block and one
excluded warmup per block. Every shared-green block is slower than every
matched assembly control block. All **512 complete-image comparisons** match
all four unpacked channels against an independent scalar formula using the
actual stock log table. These tests exclude filtering, entropy and capture;
neither shared cache passes the prerequisite for encoder integration. Status
spacing does not reverse the slowdown. The tests do not isolate its cause or
establish behavior for other geometries/formats.

## Review and reproduction

The [summary](stock-stream-followup-receipts/summary.json) contains independent
hash/event-order/warmup/median audits. The
[manifest](stock-stream-followup-receipts/manifest.json) binds every member and
SHA-256 of `evidence.tar.gz`. The archive includes measured source snapshots,
private patches, build commands, predeclared protocols, numeric logs, the
profile/annotated assembly, validation scripts and hosted source-CI receipt.
It contains no photos, RAWs, model weights or executables.

Extract into an empty directory and use the recorded compile commands with a
checkout of `3b2c0f0`, adjusting their recorded absolute paths. Source media and
prepared profiles come from the hash-bound native-input seed protocol; they
remain external. The experimental prefix function/test collectors are research
code and do not define a production ownership/error API.

I reviewed the three published FFmpeg lessons covering
[register/vector basics](https://github.com/FFmpeg/asm-lessons/blob/main/lesson_01/index.md),
[loops/addressing](https://github.com/FFmpeg/asm-lessons/blob/main/lesson_02/index.md),
and [instruction sets/alignment](https://github.com/FFmpeg/asm-lessons/blob/main/lesson_03/index.md).
Their examples use x86. These experiments use AArch64 assembly controls, NEON
intrinsics and C register packing, with output checks and Pi measurements.
They make no speedup claim based on instruction count or vectorization alone.

A subsequent [color-worker timing study](stock-preview-scheduling.md) separates
loop CPU time from a delayed fourth worker's start. Three fixed partitions do
not qualify; four workers sharing row blocks improve the matched preview
results and are adopted with unchanged pixel math, APIs and reservations.
All deadline failures and both control families remain archived.
