# Standalone native reconstruction

`gpr_reconstruct` is a macOS Objective-C++ executable for RGGB14 RAW inference
and host processing without Torch, NumPy or Core ML Tools at runtime. It uses
native Core ML, MPSGraph or the custom Metal hybrid engine. Model export remains
an explicit development step using the existing exporters and dependencies.
Runtime requires the exported artifact and the selected checkpoint's bytes
for identity checking; it does not deserialize the Torch checkpoint.

The native executable handles Bayer tiling, replicated edge padding, the
float32 bicubic baseline (`a=-0.75`, align-corners false), checkpoint residual
scale, clipping and integer conversion. The original legacy GPU bicubic/fixed
0.01 post-processing is not substituted. The native host implementation can
differ from the existing Torch host by one count at floating-point boundaries.
No backend or model quality approval follows from runtime acceptance.

## Build and raw interface

```sh
cmake -S . -B build-runtime -DGPR_BUILD_METAL_TOOLS=ON
cmake --build build-runtime --target gpr_reconstruct --parallel 4
build-runtime/source/app/stock_metal/gpr_reconstruct --help
build-runtime/source/app/stock_metal/gpr_reconstruct \
  --input decoded.raw --width 4096 --height 3072 --scale 2 \
  --checkpoint selected.pt --artifact exported-metal-model --backend hybrid \
  --kernels build-runtime/source/app/stock_metal/gpr_stock.metallib \
  --output reconstructed.raw
```

Core ML uses an artifact produced by `stock_coreml.py`, `--backend coreml` and
`--units cpu|gpu|ane|all`. Metal uses `stock_metal.py` artifacts and explicit
`mpsgraph`/`hybrid` selection. The app verifies checkpoint/artifact identity,
normalization, scale and file bytes, then holds a private model/kernel snapshot.
Core ML packages are compiled and loaded through Apple's framework. Metal
contexts use the existing bounded two-entry spatial cache. Core ML unit flags
describe allowed scheduling devices, not proven per-operation placement.

Input/output are tightly packed little-endian uint16 RGGB words in the 14-bit
count domain. Input dimensions must be even and within 16..32768; output is
limited to 268435456 pixels. The patch including halos must fit the exported
spatial limit. Higher bit domains and other CFA phases remain unsupported by
these models, independently of the stock codec's broader format support.

Both raw output and receipt names must be new. Staging and exclusive publication
protect existing files, with ordinary failure rollback and an exclusive-copy
fallback for filesystems without hard links. This is not a power-loss atomic
two-file transaction or a physical SD qualification. The receipt binds input,
checkpoint, artifact, executable, native library and output, and separately
records model load, restoration and pre-publication elapsed time.

## Standard-library media workflows

The Python control layer can also run without ML/image Python dependencies:

```sh
python -S tools/stock_still.py restore source.gpr --sample-bits 14 \
  --checkpoint selected.pt --scale 2 --sdk /path/to/gpr_tools \
  --native-executable /path/to/gpr_reconstruct \
  --native-artifact exported-metal-model --native-backend hybrid \
  --native-kernels /path/to/gpr_stock.metallib \
  --output model.dng --gpr-output restored.gpr --gpr-quality 3
```

The same `--native-*` selection works in `stock_video.py restore`,
`stock_prores.py` with a checkpoint, and captured-GPR timelapse mode of
`stock_upres.py`. Stills retain supported calibration and exact model counts
in DNG; optional GPR re-encoding requires an explicit preset and records its
separate decoded-pixel identity. GVID restoration keeps source record timing.
Direct ProRes reconstruction retains original video timing and the existing
optional PCM/timecode flow. Native upres can publish stock GPR/GVID/MOV and
calibrated ProRes, with synthetic nominal timelapse timestamps labeled as such.

Native captured-GPR upres currently supports timelapse mode. Simulated capture,
combined/regression analysis and numeric region metrics continue to use the
existing analysis workflows/dependencies; they are not silently skipped or
marked passed. Native timelapse reports `numeric_regression_evaluated: false`
and retains model/final decoded hashes and calibration accounting. A native
process is launched per frame, so repeated model-load cost is explicit. This
recreates the native inference capability through composable tools and a
standard-library control layer; the historical monolithic `gpr2prores` command
syntax is not retained.

## Host optimization and evidence

The AArch64 host path uses a separable horizontal/vertical bicubic filter,
independent register accumulators, NEON vector loads/conversion/interleaved
Bayer stores, and disjoint row workers. `--host-kernel scalar` retains the
reference algorithm; `auto` selects NEON where built. Scalar code may also be
auto-vectorized by the compiler. Scalar/NEON and one/four worker outputs must
match exactly; this is separate from agreement with Torch host arithmetic.

[Contract tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native-receipts/mac-contracts.json) cover 1x/2x, all three
engines, all four Core ML compute-unit settings, non-square/unaligned row
widths and padded tails (128x96, 130x98, 50x50). Native output differs by at most
one count from the corresponding Python/native-engine path on these fixtures.
The tests run still, GVID, timelapse and direct ProRes commands under `python -S`.
They also check scalar/vector/thread identity, existing-output protection and
corrupted artifact rejection.

[The real benchmark](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native-receipts/real.json) uses GP011728.GPR,
4096x3072 input and 8192x6144 output with the explicitly selected fresh stock
2x candidate. In the controlled cache-exercised repeat, scalar restoration
takes **0.513 s**, NEON **0.487 s** (1.052x), with identical output counts.
Whole process times are 0.843/0.610 s, but compiled caches were not flushed and
those differences are not attributed entirely to NEON. The earlier first-use
timing difference is not presented as a fivefold optimization result.
Restoration timing includes tile/context work, host conversion and mapped
output writes; model load, input checks, final flush and publication are separate.
This is one sequential pass per kernel, not sustained throughput or Pi timing.

[Real standard-library integration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native-receipts/real-integration.json)
also processes both user-provided 12 MP captures through native inference,
editable model/final DNG, stock GPR/GVID/MOV and calibrated 8192x6144 ProRes.
Original capture payloads, model count hashes, scaled calibration and packed
payloads are checked independently. [Direct ProRes integration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native-receipts/direct-prores.json)
checks native inference inside the renderer workflow rather than rendering an
already reconstructed recording. Both commands ran with site packages disabled.

Model quality remains unapproved: the selected fresh checkpoint retains its
previous held-out shadow regression. Numeric backend agreement, codec identity
and successful exports do not establish recovered detail, color/look or motion
fidelity. The camera/sensor/display/SD/thermal boundaries remain unchanged.

## Published 1x checkpoint follow-up

[Checkpoint-specific native measurements](stock-cleanup-native.md) now cover
the separately validated stock-Q3 1x candidate on two 12 MP inputs. All twelve
FP32-Core-ML/Metal cases pass their existing count budgets through `python -S`
media commands. All eight FP16-Core-ML cases exceed the predefined two-count
budget despite faster execution. FP32 is the validated Core ML configuration.
These checks do not approve the earlier 2x model, wider domains or video quality.

## Supplied 2x diagnostic follow-up

The later [source-wide 2x diagnostic](stock-sr-native.md) passes all twelve
standalone execution cases and 24 full-reference cases on FP32 Core ML,
MPSGraph and hybrid, within existing count budgets (observed maximum one count).
Every declared RAW region retains nonregression against the frozen MPS bicubic
baseline. The diagnostic's small +0.00729 dB pooled benefit does not establish
useful recovered detail or broader model/temporal approval. These results are
checkpoint-specific and do not change the earlier candidate's recorded failure.

## Supplied model in the complete video/export chain

The [new two-frame integration](stock-selected-video-chain.md) uses the supplied
2x checkpoint with a fresh native-geometry Pi streaming recording. It verifies
native versus Torch counts, identical native counts across media workflows,
supported calibration, independent Q5 payloads, exact private MOV recovery and
a decodable 8192x6144 ProRes HQ movie with original relative capture timing.
This supersedes the older-checkpoint integration as evidence for the supplied
model's complete chain; it does not relabel that model's quality scope. Native
ProRes receipts now include their actual reconstruction settings.
