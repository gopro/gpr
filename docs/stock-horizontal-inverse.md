# Horizontal inverse SIMD experiment: discarded

An explicit eight-position NEON horizontal inverse is exact and its second
version is **9.05% faster in the isolated Pi kernel benchmark**, but it does
not establish a consistent combined recording/preview benefit. All four
native-input 600-attempt comparisons fail the strict floor. The runtime
proposal is discarded; production source and executable are unchanged.

## Diagnostic and arithmetic proposal

A separate diagnostic samples the production capture executable at 499 Hz
using user cycles and DWARF call stacks, with the actual native-resolution
GP011728/GPAB1094 inputs from the [preceding timing check](stock-native12-gopro-timing.md).
Capture runs as UID 1000 with zero effective capabilities, FIFO priority 1 and
nice 0; the privileged collector samples only user cycles. It finishes with
9,995 samples and zero reported lost samples. All 113 accepted payloads,
calibration records and preview images match independent references; seven
attempts overrun. Profiled latency is diagnostic, not acceptance evidence.

The [flat profile](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/profile/flat.txt) assigns
35.45% to incremental highpass encoding, 15.07% to channel unpacking, 12.05% to
forward transform, 8.40% to display rows and 8.20% to the spatial inverse.
These are process-wide CPU sample shares, not critical-path percentages or
predicted wall-time gains. The
[protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/profile/protocol.json),
[verified receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/profile/receipt.json),
[call graph](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/profile/callgraph.txt) and
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/profile_stock_native12_preview.py)
retain the diagnostic separately from unprofiled comparisons.

The tested kernel replaces only the interior of `InvertHorizontalDescale16s`.
Eight input positions produce sixteen interleaved output values. Four-lane
halves are independent in vector registers; signed widening preserves the
scalar intermediate range and saturating narrowing preserves `ClampPixel`.
Borders and scalar tails are unchanged. Loads/stores require only the existing
16-bit alignment, with neither aligned-row assumptions nor widened overreads.

Version 1 explicitly adds the rounding bias and shifts. Version 2 uses ARM's
rounded shift-and-accumulate operation for the identical `(delta + 4) >> 3`
term. The [compiled assembly](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/v2/horizontal-assembly.txt)
contains four `srsra` instructions in that vector path. It is an explicit
register-level/SIMD comparison, not an assumption that the original compiler
emitted no vector instructions.

## Independent correctness and isolated timing

An independent oracle uses floor division rather than right-shift expressions,
then signed saturation. Six thousand cases compare 6,123,184 output samples
against both the candidate and the exact original helper. Cases include both
descale modes, widths in 3..1025, odd output widths, eight pointer offsets,
extreme signed inputs and randomized data. Whole output arrays are compared,
including guard regions; source arrays must remain unchanged. The oracle and
four existing retained-band probe/matrix targets pass on Mac and Pi for both
experimental versions. The matrix covers the existing CFA/depth/preset/odd-size
contracts; these five experimental checks do not replace the published broader
production validation.

Each isolated Pi process pins itself to CPU 1 and benchmarks both helpers in
the same executable, in A/B/B/A/B/A/A/B order. Each section processes 153,600
rows, width 512, with matching checksums. The original helper is compiled by
the same compiler as the candidate. Medians below summarize the four sections
per helper within each executable; these are kernel timings, not frame times.

| Version | Original median seconds | Candidate median seconds | Candidate change |
|---|---:|---:|---:|
| Explicit bias and shift | 0.0993701 | 0.1069094 | 7.59% slower |
| Rounded shift-and-accumulate | 0.0994310 | 0.0904337 | 9.05% faster |

[Version 1 events](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/v1/pi-oracle-benchmark.stdout),
[version 2 events](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/v2/pi-oracle-benchmark.stdout)
and the [decision summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/summary.json)
retain both outcomes. Mac timings are separately archived and do not establish
a Cortex-A76 speedup. Decoder, encoder and capture compiler flags match the
production build after normalizing source directory names.

## Unprofiled native-input combined comparison

The fixed [ABBA protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/capture-abba/protocol.json)
compares production against version 2: 600 attempts per run, native 4096x3072
RGGB14 Q3 inputs, 20 fps, leased source buffers, every-frame calibrated preview,
encoder-derived reconstruction, serialized compute, foreground preview,
1 MiB writeback and final fsync. Input bytes, typed calibration, independent
stock payload hashes and offline preview CRCs are the preceding native study's
frozen references. The optional Linux process profile remains FIFO 1/nice 0;
all four capture UIDs are 1000 with zero effective capabilities.

No unrelated Pi build, test or profiler runs during these four measured
children. Independent verification occurs only after each capture finishes.
The executable hashes and candidate source hash are frozen before timing.
The [driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/benchmark_stock_horizontal_capture.py)
and [full receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/capture-abba/receipt.json)
preserve every run:

| Order | Recorded / attempted | Durable fps | Encode/reconstruct median ms | Render median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|---:|
| Original A | 560 / 600 | 18.5850 | 40.3895 | 9.49781 | 560 | 560 |
| Candidate B | 600 / 600 | 19.9972 | 35.3030 | 9.24813 | 4 | 2 |
| Candidate B | 558 / 600 | 18.5072 | 40.4795 | 9.67180 | 558 | 558 |
| Original A | 600 / 600 | 19.9913 | 35.8669 | 9.19498 | 10 | 90 |

All 2,318 accepted payloads, source calibration records and delivered preview
images match independent stock/offline references. Attempt associations,
queue accounting and END3 verify. There are 82 overruns across all four runs;
exact accepted data does not make those dropped attempts successful. Both
recording and preview floor flags are false for every run, including the
near-passing candidate. Its final durable fps remains below 20.

Both binaries exhibit a faster and slower run. The candidate has better late
counts in its faster run, but its slower run does not beat the slower control.
This does not establish a repeatable workflow gain or isolate the source of
the variability. The isolated SIMD win therefore does not justify changing
production or declaring native-input preview timing fixed. No new source,
model or camera scheduling option is adopted.

## Disposition and evidence boundaries

The isolated Mac checkout's tracked files are restored and its experimental
test is removed. The main Pi source was never modified; its
[production binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/production-unchanged.json)
verifies the inverse, reconstruction, renderer and capture source hashes against
the published branch, plus the original executable
`c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`.
Global realtime limits remain 950000/1000000 microseconds. The earlier June
processes remain running and untouched. No device/controller or boot setting
changes. All diagnostic and comparison children have finished.

The archived CMake patches and test harnesses describe rejected experiments;
the extra oracle is not a newly registered production target. Each version's
patch applies to `9111564`; copy its archived `horizontal_inverse_test.c` into
`tests/` before configuring. Large binaries, raw GVIDs and the 85.5 MB perf
recording remain external under the corresponding Pi experiment directories
and `/Volumes/OWC_8TB/gpr_work/artifacts/stock_horizontal_inverse_20261030`.
Some artifact hashes bind those external binaries/full disassemblies rather
than files redistributed in the public receipt directory. Actual source photos
and full calibration stay external. These are SSD replay/memory-sink tests,
with no camera CNN or sensor/display/thermal qualification.

All seven hosted jobs pass for the preceding published revision `9111564`
([run 41163202174](https://github.com/dcliftreaves/gpr/actions/runs/41163202174),
[receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-horizontal-inverse-receipts/passed-9111564.json)). This evidence
update does not change production source; its own hosted status is separate.
The next timing work needs to resolve the variable combined critical path,
rather than assume an isolated inner-loop improvement translates into a
camera-rate guarantee.
