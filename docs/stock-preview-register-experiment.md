# Rejected CFA canonicalization and expanded preview coverage

The runtime change tested here was **discarded**. The retained change is stronger
independent preview validation: RGGB, GBRG, GRBG and BGGR in 12-, 14- and 16-bit
count domains. Two source widths, three reductions, eight orientations and
twelve CFA/depth combinations give 576 calibrated configurations per test
invocation. The fixtures check gain maps, sequential clipping, fractional black
levels, scaled white levels, crops, output padding and direct/serial-decoder
agreement against independent stock linear-RGB data. ARM builds also run the
scalar renderer. This does not widen the CNN's separate RGGB14 contract.

## Experiment and decision

The prototype resolved CFA-site ordering at immutable-plan creation. Gains,
clipping caps, black values and inverse ranges were stored in canonical
R/G1/G2/B order, letting render kernels use constant site indices. This removed
runtime vector-array selection and four private plan fields. Public calibration
and stock payloads were unchanged.

The native AArch64 `display_wavelet_rows` function shrank from 2332 to 2180 bytes.
Static `STP Q` vector-pair store sites fell from 15 to 2. Those are emitted-code
counts, not executed instruction counts, complete spill counts or cycle costs.
The [assembly receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/assembly.json) and
[previous](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/previous.asm)/
[prototype](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/canonical.asm) disassembly preserve
the measured binaries.

The fixed-profile isolated renderer was slower despite that assembly change:

| Previous/prototype order | Render median, ms | p95, ms |
|---|---:|---:|
| Previous | 6.21208 | 6.88101 |
| Prototype | 6.57932 | 7.02847 |
| Prototype | 6.60673 | 7.04249 |
| Previous | 6.25428 | 6.82688 |

Each run renders 400 full 1024x768 frames from two frozen 12 MP sources. Decode,
plan setup and CRC computation are outside render timing. The calling process
runs in the foreground, with no capture encoder, display queue or storage
writer. Both source RGB CRCs match the independent offline preview throughout.
The [harness](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/kernel_bench.cpp) and
[compiler commands/results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/kernel.json) are
retained. This measurement is not an end-to-end capture timing gate.

## Sustained control

The preceding matched previous/prototype/prototype/previous runs each attempted
600 frames at 20 fps, 4096x3072 RGGB14/Q3, encoder-derived quarter preview,
memory CRC sink and page-aligned 1 MiB rolling writeback plus final fsync.
Background processes were left unchanged.

| Order | Recorded / previewed | Durable fps | Preview render median, ms |
|---|---:|---:|---:|
| Previous | 509 / 509 | 16.8985 | 38.7205 |
| Prototype | 513 / 513 | 16.9902 | 37.6459 |
| Prototype | 504 / 504 | 16.6973 | 38.8479 |
| Previous | 513 / 513 | 17.0088 | 37.6188 |

Every accepted payload, source calibration and delivered preview matches the
independent stock reference, but all runs overrun and fail the strict floor.
There is no consistent sustained improvement. Combined with the isolated
slowdown, this supports discarding the runtime change. The experiment also
changed private plan layout; these results do not isolate a single compiler or
cache cause. Fewer static stores alone are insufficient evidence of a speedup.

The [full replay receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/rejected-abba.json),
[rejected patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/rejected-canonical.patch) and
[prototype source hashes](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/rejected-sources.json)
preserve the experiment against base `a7e59f6`. Its
[package checks](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/rejected-packages-mac.json)
belong to the rejected prototype and are not presented as current-runtime
performance evidence. Apply the patch only to that base to reproduce it; the
current branch retains the original runtime implementation.

## Retained verification

After reverting the prototype, the expanded twelve-format preview checks pass
in the [24-target Mac Release suite](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/mac-release.txt)
and [21-target Pi Release suite](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/pi-release.txt).
The [validation receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/validation.json) binds
current source, rejected source and evidence separately. The public API, default
scheduling, encoder, compact unity-cap optimization and writeback policy remain
those already implemented at the base revision. Sustained timing and model
quality gates remain unmet; no camera-production or model promotion follows.

The preceding hosted run at `a7e59f6` passed six jobs, but its Mac job never
acquired a hosted ARM64 runner. GitHub reports capacity constraints and no build
or test steps ran. The [run identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/hosted-run.json)
and [annotations](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-canonical-preview-receipts/mac-capacity-annotations.json)
preserve that distinction. Local Mac results do not turn that hosted gate into
a pass; the new head must retry it.
