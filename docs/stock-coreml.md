# Optional native Core ML reconstruction

`stock_coreml.py` restores the applicable Core ML inference option from the
original RAW media workflow. It executes the bundled BIBO residual network
through native Core ML and retains the current Torch host baseline, Bayer
tiling, normalization, clipping and integer rounding. Both 1x cleanup and 2x
reconstruction are supported. This Python integration still requires Torch;
it does not recreate the old standalone Objective-C app, MPSGraph implementation
or custom Metal hybrid kernels. The [standalone native runtime](stock-native-runtime.md)
provides a separate Core ML option without Torch/Core ML Tools at runtime.

The original `SuperResCNN.m` at runtime revision
`8be29eee57b4637865b572f18ab9e3f4edaa2236` also separated native residual
inference from the host baseline. The new adapter reads residual scale from
the selected checkpoint rather than assuming a fixed 0.01. It uses the
stock-input checkpoint/architecture contract; no old model approval transfers.

## Export and use

Core ML is optional and macOS only. The camera libraries and portable Python
paths do not import Core ML Tools. Locally tested dependencies are Torch 2.8.0
and Core ML Tools 9.0. Native ML programs target macOS 13 or later.

```sh
python -m pip install torch==2.8.0 coremltools==9.0
python tools/stock_coreml.py selected.pt --output-dir new-native-model \
  --precision float32 --max-plane 512
python tools/stock_still.py restore source.gpr --sample-bits 14 \
  --checkpoint selected.pt --coreml-package new-native-model --coreml-units gpu \
  --sdk /path/to/gpr_tools --output restored.dng --scale 2
```

The same `--coreml-package` and `--coreml-units` options are available on
`stock_video.py restore`, `stock_upres.py` and CNN-enabled `stock_prores.py`.
Keep `--device cpu`: it selects host tensors, while Core ML units select
eligible native execution devices. Existing Torch CPU/MPS/CUDA paths remain
available when no native package is selected.

`cpu` permits CPU only; `gpu` permits CPU/GPU; `ane` permits CPU/Neural Engine;
`all` permits all available units. These are permissions for Core ML's runtime
scheduler, not proof of per-operation placement. See Apple's
[compute-unit and conversion documentation](https://apple.github.io/coremltools/docs-guides/source/new-conversion-options.html).
No automatic Torch retry hides a selected native-runtime failure.

The export directory contains `model.mlpackage` and `contract.json`. The
contract binds checkpoint bytes, bundled architecture bytes, output scale,
residual scale, count normalization, CFA, precision, spatial limits and all
package files. Runtime verifies these identities before loading the package.
Package symlinks and existing export destinations are refused. Identity hashes
detect mismatches; they are not a signature or an independent quality review.

Input planes must be CPU float32, batch one, four Bayer sites, padded to
multiples of eight within the exported limit. Include both context halos when
choosing that limit: tile 256 plus two 64-pixel halos fits a 512-pixel export.
The current models require RGGB14. External architecture overrides are not
accepted by this adapter. Each native result must have the declared shape and
finite residuals. Raw source metadata follows the existing still/video
calibration and 2x geometry contracts.

## Evidence and limits

[Synthetic contracts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-coreml-receipts/mac-contracts.json) cover both scales,
float32/float16 and all four unit settings, with non-square/tiled edge shapes.
On those fixtures the native output differs from Torch by at most one 14-bit
count. Tests also exercise checkpoint/package/scale binding, export collisions,
spatial rejection, editable still/GPR output, GVID restoration and calibrated
ProRes. Hosted macOS repeats the optional suite. Package checks verify imports
without Torch/Core ML, and ordinary portable contracts run on Mac and Pi.

The real-source benchmarks use `GP011728.GPR` (4096x3072), the explicit fresh
2x stock candidate, 8192x6144 output, four host Torch threads, tile 256 and halo
64. They compare the same checkpoint's backend output, not independent sensor
truth. See [float32](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-coreml-receipts/real-fp32.json) and
[float16](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-coreml-receipts/real-fp16.json) receipts. The default float32
path differs by at most one count. Experimental float16 differs by up to
26 counts on this real scene despite the smaller synthetic differences.
Precision/backend agreement needs scene-specific validation; neither result
approves this candidate, which still has its previously measured shadow
regression. No temporal, rendered-look or sustained throughput approval follows.

Measured on the local M5 Max Mac, with separate sequential comparison runs:

| Export precision | Torch restoration | Core ML restoration across allowed-unit settings | Maximum difference from Torch |
|---|---:|---:|---:|
| Float32 (default) | 1.891 s | 1.287–1.312 s | 1 count |
| Float16 (experimental) | 1.682 s | 0.899–0.919 s | 26 counts |

These Mac timings are not Pi performance estimates. Native Core ML is not
available in the Pi's Linux runtime. Each row has its own measured Torch
baseline; the variation is another reason not to treat a single pass as a
sustained performance guarantee.

[Real upres integration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-coreml-receipts/real-upres-integration.json)
also verifies that the selected native output counts reach stock GPR/GVID/MOV,
with scaled camera calibration and original capture-payload preservation.
Counts match the independent native-backend benchmark. Core ML output may
differ from Torch counts, so compressed payload identity is asserted relative
to the actual counts supplied to the stock encoder, not across backends.

```sh
python tools/benchmark_stock_coreml.py --source source.gpr \
  --checkpoint selected.pt --coreml-package new-native-model \
  --sdk /path/to/gpr_tools --threads 4 --output-dir new-comparison
```

This measures one inference pass per setting; model loading is separate,
compiled-model caches are not flushed, and interpreter/Torch startup is not
included. The restoration-loop timer covers tiled inference, host baseline
and output count conversion. Repeated or sustained timing needs its own run.
