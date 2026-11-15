# Rejected 64-bit entropy chunk experiment

A corrected register-level packing prototype preserves codec bytes but does not
improve the measured combined Pi recording/preview workload. Neither candidate
is retained. Production source, defaults, public APIs, weights and the existing
capture executable remain unchanged; the strict sustained deadline gate is open.

## Hypothesis and the boundary caught by integration

The existing incremental highpass encoder combines four independent signed
code-table loads into one chunk when their total length is at most 32 bits.
Longer groups append four tokens serially. The prototype joins groups up to
64 bits and flushes one or two 32-bit words. It uses balanced register shifts,
not a new codebook, pair lookup table or bitstream profile. A hybrid keeps the
original <=32-bit branch and applies the new path only to 33..64-bit groups.
Groups longer than 64 bits retain the original per-token fallback.

The initial `v1` draft passes 526,336 independent 128-bit reservoir states and
115,136 stream comparisons covering initial occupancies **0..31**. It nevertheless
fails the ordinary encoder preview matrix on a GBRG16 case; the optimized matrix
passes. A repeat reproduces the failure while the unchanged control passes.
The failed draft and its narrower proof remain archived, not treated as accepted
compatibility evidence. Its synthetic timings are superseded by corrected v2.

The codec can validly retain **32 buffered bits**. At occupancy 32 and group
length 64, the new head expression would shift a 64-bit operand by 64, which C
does not define. V2 explicitly contributes zero for that head fragment and
preserves the buffered head word. Up to 32 residual bits remain valid; the
independent wide-integer oracle models the at-most-two-word flush accordingly.
The corrected harness includes every initial occupancy 0..32, lengths 1..64,
257 randomized states per pair, seven coefficient patterns, padded row tails,
zero runs, extremes and incremental row splits, with output canaries. It checks
all 65,536 signed-code lengths against the 1..32-bit proof contract.

Both corrected candidates pass **542,784 reservoir states and 118,734 complete/
incremental stream comparisons** on Mac and Pi. Fresh baseline/full64/hybrid
Pi builds each pass all four retained-band probe/matrix tests, including the
previously failing ordinary case. This preserves the independent inverse/count
and historical codec checks over declared CFA/depth/quality/tail combinations.
No sanitizer campaign is part of this experiment.

## Synthetic and real-frame results

The corrected full64 microbenchmark improves medium coefficient packing by
about 10.8%, but small coefficients regress about 7.4%. The corrected hybrid
keeps small-coefficient timing near its control and improves medium coefficients
about 12.2%, while its larger-coefficient case regresses about 5.0%. Each is a
same-binary, CPU-2-pinned synthetic comparison with fixed inputs and eight
alternating runs per pattern; these are not whole-encoder speedups.

A fixed baseline/full64/hybrid/hybrid/full64/baseline family then encodes two
real 4096x3072 RGGB14 Q3 inputs, 31 reused-arena iterations per process, excluding
the first from medians. This uses separate matched compile-time variants with
no runtime dispatch. All twelve final payload SHA-256s match the independent
historical encoder; all 31 logged MD5s remain stable in each process.

| Source | Baseline median of run medians | Full64 | Hybrid |
|---|---:|---:|---:|
| GP017517.raw | 26.590 ms | 26.853 ms | 26.251 ms |
| GP017348.raw | 19.726 ms | 19.404 ms | 19.470 ms |

The hybrid's standalone median improves approximately 1.3% on both inputs.
The full64 result is mixed. These scoped encoder costs exclude output-file
writes/MD5 work and do not establish capture or deadline headroom.

## Combined capture/preview decision

Baseline/hybrid/hybrid/baseline runs attempt 600 frames each at 20 fps, with
both frozen source/calibration pairs, leased inputs, every-frame 1024x768
encoder-derived preview, the memory CRC sink, serialized compute, page-aligned
1 MiB rolling writeback and final fsync. Both fresh builds use the same source
and configuration except the entropy variant. All 2,400 recorded payloads,
calibrations, delivered preview pixels, source/timestamp bindings and final
markers match their independent historical/offline references.

| Implementation | Durable fps | Encode/reconstruct median, ms | Render median, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| baseline | 19.9945 | 40.5480 | 7.9600 | 445 | 508 |
| hybrid | 19.9384 | 39.6261 | 9.6738 | 526 | 562 |
| hybrid | 19.9495 | 39.1968 | 8.7565 | 152 | 317 |
| baseline | 20.0021 | 39.5874 | 8.2415 | 196 | 360 |

Both hybrid runs have lower durable throughput and slower render medians than
both controls, despite generally lower encoding medians. Late-delivery counts
are mixed, and every run fails the strict zero-late gate. Pacing caps throughput
near 20 fps; do not interpret tiny fps differences as a general codec regression
or attribute render variation solely to the changed entropy code. The measured
standalone improvement does not establish an end-to-end benefit, so the proposal
is rejected. Existing background workloads are unchanged. Recorded clocks are
2.4 GHz/performance; before/after temperatures are observations, not thermal
control. No additional full package rebuild is claimed for this rejected draft.

## Evidence and reproduction

[Summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/summary.json),
[corrected full proof/timing](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/v2-full/receipt.json),
[corrected hybrid proof/timing](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/v2-hybrid/receipt.json),
[build commands/identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/integrated/builds-v2.json),
[real-frame protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/integrated/frames/protocol.json),
[real-frame measurements](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/integrated/frames/receipt.json),
[capture protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/integrated/capture/protocol.json),
[capture verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/integrated/capture/receipt.json) and
[corrected integrated patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy64-receipts/integrated/prototype-v2.patch)
retain the complete checked result. Corrected/initial micro directories include
exact extracted functions, independent harnesses and source/binary identities;
initial failures and Mac proof outputs remain distinct.

The integrated patch applies to `9e51ea1`; its reconstructed source SHA matches
both measured protocols, and its two candidate blocks match the micro harnesses.
Build mode 0 is unchanged packing, 1 is full64 and 2 hybrid, selected with
`-DGPR_ENTROPY64_VARIANT=N` in the recorded C flags. These macros are experimental
artifacts, not supported production options. The integrated directory supplies
`build.py`, `run-frames.py` and `run-capture.py`. Copy the existing direct-output
command template as `capture-command-template.json` when reproducing, adapt
original installation/data paths in a fresh output root, and regenerate bindings.

The micro protocol records compiler arguments and the CPU-pinned command. Use
its matching `test.c` and `functions.inc` against the recorded codec libraries;
`--bench` runs synthetic timings after all correctness checks. RAW sources,
compiled binaries, payloads and full GVID outputs remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_entropy64_20261030` and matching Pi
directories. No camera sensor/display/card, broader model quality or real-time
CNN approval follows from these tests.
