# Optional MPSGraph and custom Metal reconstruction

The stock workflow now offers the original direct MPSGraph and hybrid Metal
residual engines in addition to Torch and Core ML. Graph construction, BN
folding, weight layout and the three ANE-style custom kernels derive from
`tools/gpr2prores` at PR #95 runtime revision
`8be29eee57b4637865b572f18ab9e3f4edaa2236`.

The adapter deliberately uses the current host baseline, checkpoint residual
scale, tiling and integer conversion. The old GPU post-processing used a
different bicubic polynomial and a hardcoded 0.01 scale; it is not reused.
This is a residual-engine port, not a reconstruction-quality approval or the
old standalone Objective-C application. This Python adapter retains Torch host
processing; a [standalone native option](stock-native-runtime.md) now removes
that runtime dependency through a separate raw executable and standard-library
media adapters.

## Build, export and select

Build on macOS with Xcode and its Metal compiler available:

```sh
# If Xcode reports that the Metal Toolchain component is missing:
xcodebuild -downloadComponent MetalToolchain
cmake -S . -B build-runtime -DGPR_BUILD_METAL_TOOLS=ON
cmake --build build-runtime --target gpr_metal --parallel 4
python tools/stock_metal.py selected.pt --output-dir new-metal-weights
python tools/stock_still.py restore source.gpr --sample-bits 14 \
  --checkpoint selected.pt --scale 2 --sdk /path/to/gpr_tools --output restored.dng \
  --metal-weights new-metal-weights \
  --metal-library build-runtime/source/app/stock_metal/libgpr_metal.dylib \
  --metal-kernels build-runtime/source/app/stock_metal/gpr_stock.metallib \
  --metal-backend hybrid
```

The same selection options work with `stock_video.py restore`, `stock_upres.py`
and CNN-enabled `stock_prores.py`. `mpsgraph` runs the full residual graph;
`hybrid` uses MPSGraph for the surrounding layers/middle block and the original
custom fused kernels for the six encoder/decoder blocks. The latter retain
SiLU/projection intermediates in float registers rather than rounding every
intermediate to half. They use the original width-16 limits: block C at most
64, with 2C at most 128 values in the thread-local register array.

Keep `--device cpu` for host tensors. Native engine selection is separate.
Metal and Core ML selections are mutually exclusive; a selected engine's
failure is returned rather than silently changing engines. Camera-library
profiles have no Apple framework dependency. This optional engine is not
available on Linux/Pi. Installed artifacts are `lib/libgpr_metal.dylib` and
`lib/gpr/gpr_stock.metallib`, with the Python adapter under `share/gpr/tools`.

The exported package binds the selected checkpoint, bundled architecture,
1x/2x scale, residual scale and every BN-folded fp16 weight/manifest file.
Runtime checks those identities before native loading; extra/truncated/changed
files and symlinks fail. Native loading also checks fixed-width bias/conv
sizes and bounded file reads. The private C bridge validates input/output
counts, finite normalized input, GPU errors and finite output. Its contexts
are serialized by the adapter and released by a bounded two-entry spatial LRU.
Validated weight and metallib bytes are held in a private temporary snapshot;
later cache misses cannot pick up replacements of the external artifacts.
Closing a model releases contexts and the snapshot, and rejects further calls.

Only the current width-16, depth-3, 3x3 F_ane/BIBO 1x/2x contracts are supported.
Input is normalized float32 NCHW, batch one, four RGGB sites; native storage
is fp16 NHWC. Spatial patch extents must be multiples of eight within 8..512,
including halos. Raw input remains RGGB14. Wider/LN/7x7 models are not advertised
by this adapter. Weights stay external and no model is selected automatically.

## Evidence and limits

[Synthetic contracts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-metal-receipts/mac-contracts.json) cover both
engines/scales, tiled edges, context eviction/reuse, malformed artifacts,
native rejection of a truncated bias, device/backend selection, editable
still/GPR, GVID restore, upres and calibrated ProRes. The test uses residual
scale 0.5 to expose layer/channel errors and a hidden fixed-0.01 assumption.
Observed maxima versus Torch are 6–7 counts; peer-engine differences reach
5 counts. These engines are not bit-identical. A separate zero-residual test
retains exact current bicubic/count output. The numeric test guard is an
engineering contract, not a model-quality threshold.

[The real-source receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-metal-receipts/real.json) uses the declared
fresh stock 2x candidate and `GP011728.GPR` (4096x3072), producing 8192x6144
counts on the local M5 Max Mac. Four Torch host threads, tile 256, halo 64:

| Engine | Restoration loop | Speed relative to this Torch run | Maximum difference from Torch |
|---|---:|---:|---:|
| Torch | 1.657 s | 1x | Reference |
| MPSGraph | 0.783 s | 2.12x | 8 counts |
| Hybrid Metal | 0.786 s | 2.11x | 7 counts |

Native RMSE versus the same checkpoint's Torch output is 0.326/0.324 counts.
This is a cache-exercised follow-up with one measured pass per engine, not
a cold-start or sustained-throughput guarantee. Compiled-model/kernel caches
were not flushed. Context creation on spatial cache misses is included in
the restoration timer; interpreter/model load is separate. First-use cost can
be materially higher. These Mac measurements do not estimate Pi performance.

[Real upres integration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-metal-receipts/real-upres-integration.json)
checks matching native model counts, scaled source calibration including
GainMaps, exact GVID payloads/END3, MOV recovery and completed calibrated
ProRes from the final GPR-derived frames. It does not run a second CNN pass.
Backend agreement and encoding/export correctness do not establish recovered
detail, rendered appearance or motion fidelity. The selected candidate's
previous held-out shadow regression remains; no new model approval follows.

```sh
python tools/benchmark_stock_metal.py --source source.gpr \
  --checkpoint selected.pt --weights new-metal-weights \
  --library /path/to/libgpr_metal.dylib --kernels /path/to/gpr_stock.metallib \
  --sdk /path/to/gpr_tools --output-dir new-comparison
```
