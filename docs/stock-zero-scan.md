# Bulk zero-run scanners: rejected

Two private alternatives to the encoder's scalar zero-prefix loop preserve
correctness but are slower on the Pi. Neither is adopted. Production retains
the scalar loop, the `f1e08c8` encoder/reconstruction core and `f197f09` preview
spacing. This experiment does not close the sustained timing gate.

## Rationale and implementations

The [current sampled assembly](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-balance-receipts/sampled/entropy-assembly.txt)
shows substantial local sample weight in the scalar zero-run search inside
incremental highpass encoding. The hypothesis is to skip all-zero coefficient
groups with fewer loop iterations while leaving VLC packing and pending run
state unchanged.

The word variant checks four 16-bit coefficients through an unaligned-safe
`memcpy` into a 64-bit register. The NEON variant checks eight unsigned 16-bit
lanes with a vector load and maximum reduction. Both skip only entirely zero
groups and finish the remaining prefix with the original scalar search. Bounds
checks require a complete group before each wide load. Coefficient rows may be
only two-byte aligned; no stronger alignment or readable padding is assumed.
The NEON experiment falls back to scalar outside AArch64.

I reviewed the three published [FFmpeg assembly lessons](https://github.com/FFmpeg/asm-lessons):
[register/vector basics](https://github.com/FFmpeg/asm-lessons/blob/main/lesson_01/index.md),
[loop/address calculations](https://github.com/FFmpeg/asm-lessons/blob/main/lesson_02/index.md),
and [instruction sets/alignment](https://github.com/FFmpeg/asm-lessons/blob/main/lesson_03/index.md).
They teach x86 examples; this prototype applies the general loop, vector and
alignment considerations to AArch64 rather than treating x86 instructions as
Pi instructions. The archived Pi disassembly confirms an actual vector load
and `umaxv` in the NEON entropy function. These are intrinsic/compiler prototypes,
not handwritten assembly kernels or claimed gains from the guide.

## Correctness

An independent prefix-position test checks 674,298 cases in each build:
every two-byte alignment modulo 128, short lengths and first-nonzero positions,
every nonzero 16-bit representation in all eight vector lanes, and rows touching
inaccessible guard pages at both ends. It checks empty inputs without requiring
a readable element. Mac word/NEON and Pi scalar/word/NEON builds all pass.
Each relevant build also passes 14 registered codec/preview tests, including
four CFA phases, three depths, six qualities and odd packed output tails.

The separate real-input benchmark checks each encoded payload against frozen
independent bytes. All 492 payloads match. This supplies codec integration
evidence beyond the prefix test; no approximate zero classification or changed
quantization is involved.

## Pi measurements

Fresh builds use the same source and Release options except
`GPR_ZERO_SCAN=0/1/2`. Fixed order is scalar/word/NEON/NEON/word/scalar, with both
frozen 12 MP inputs per process group. Each process performs 41 Q3 encodes,
excluding its first five from the reported median. This uses the public encoder
with per-encode allocation/free, no preview or writer; it is not capture timing.

| Input | Scalar medians ms | Word medians ms | NEON medians ms |
|---|---|---|---|
| 0 | 25.001 / 25.444 | 25.934 / 25.644 | 26.945 / 26.641 |
| 1 | 18.966 / 20.107 | 20.246 / 21.051 | 21.954 / 22.599 |

Both alternatives are slower than both scalar controls for each source. The
result does not establish why: the relative costs of short prefixes, extra
loads, branches and reduction instructions are not separately measured here.
No full capture comparison follows because neither meets the isolated
performance prerequisite. A future proposal should first measure actual prefix
lengths and preserve the existing cheap nonzero/short-run paths.

## Reproduction and restoration

[Build commands, proofs and identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-zero-scan-receipts/builds.json),
[standalone protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-zero-scan-receipts/standalone/protocol.json),
[verified results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-zero-scan-receipts/standalone/receipt.json),
[Pi build driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-zero-scan-receipts/build.py) and
[measurement driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-zero-scan-receipts/benchmark-standalone.py)
retain the exact scope. The receipt directory contains raw samples, test logs,
per-variant entropy disassembly, the private patch/header and proof/benchmark
sources. Apply `prototype.patch` to `950deda`, put `zero_scan.h` in
`source/lib/vc5_encoder/` and the two extra C files in `tests/`, then adapt the
recorded input/build/output paths. Use fresh measurement output roots.

Large binaries/media remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_zero_scan_20261030` and the matching
Pi experiment directory. Local tracked prototype files are restored to
`950deda`; its own untracked source files are removed after archive comparison.
Only experimental build directories remain. Production source, public API,
model weights and claimed quality tiers are unchanged.
