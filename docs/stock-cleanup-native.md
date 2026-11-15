# Published 1x checkpoint through native desktop backends

The [published stock-Q3 1x checkpoint](../models/stock-q3-rggb14-cleanup/README.md)
now runs through standalone Core ML, MPSGraph and hybrid Metal on both supplied
12 MP sources. Each media command uses `python -S`: Torch, NumPy and Core ML
Tools are absent from the runtime control path. Export and independent result
analysis still use their normal development dependencies.

Checkpoint SHA-256 remains
`9de03f8203f4cb1bb264087cd922b04c1b0e1169684a064d0aeb3a2a6c97f48a`.
Neither weights nor residual scale are changed for these backend checks.

## Counts, metadata and measured time

The comparison uses the same frozen 4096x3072 RGGB14/Q3 inputs and Torch-MPS
reference outputs as the [portable study](stock-cleanup-strength.md). A fresh
SDK decode must match reference input counts. Independent TIFF reads must
match reconstructed pixel hashes, and supported calibration must match exactly.
Native output uses scale one, tile 256, halo 64 and four host workers.

| Backend / artifact | Cases | Maximum count difference from Torch MPS | Native restore time, s | Result |
|---|---:|---:|---:|---|
| Core ML FP32; cpu/gpu/ane/all policies | 8 | 1 | 1.792–1.852 | Passes predefined one-count bound |
| MPSGraph; FP16 exported weights | 2 | 2 | 1.694 / 0.408 | Passes existing Metal count bound |
| Hybrid Metal; FP16 exported weights | 2 | 2 / 1 | 0.420 / 0.411 | Passes existing Metal count bound |
| Core ML FP16; cpu/gpu/ane/all policies | 8 | 3 / 4 by source | 0.599–0.607 | **Fails predefined two-count bound** |

The existing Metal test bound is 32 counts; observed error here is at most
two, not a new universal two-count guarantee. Core ML's FP16 path is faster in
these measurements but is not validated within the selected precision budget
for this checkpoint. FP32 remains the validated Core ML configuration.

Restore time includes native tile/context execution, host conversion and raw
output writes; model loading, initial input validation, final publication,
SDK decoding and editable DNG packaging are separate. Entire successful media
processes take 2.125–2.182 s for Core ML FP32, 1.968/0.667 s for MPSGraph and
0.685/0.678 s for hybrid in this ordered experiment. First-use costs and caches
are not controlled, so the two MPSGraph times do not establish sustained
throughput. Device-policy flags allow scheduling devices; they do not prove
which device executed each operation or establish an ANE speedup.

All twenty commands complete with checked pixels and exact supported
calibration, including the eight FP16 commands that fail their count bound.
The twelve FP32-Core-ML/Metal checks pass their respective numeric budgets.
These are still-derived replay inputs, not new lossless sensor references.
The four-reference quality scores remain the Torch-MPS result; backend count
agreement does not independently approve visual, temporal or broader-camera
quality. The checkpoint's general approval flag stays false.

## Explicit export and use

With the optional export environment (measured Torch 2.8.0/Core ML Tools 9.0):

```sh
python tools/stock_coreml.py models/stock-q3-rggb14-cleanup/selected.pt \
  --precision float32 --max-plane 512 --output-dir /new/coreml-f32
python tools/stock_metal.py models/stock-q3-rggb14-cleanup/selected.pt \
  --output-dir /new/metal
```

Then run the standard-library frontend with the standalone native executable:

```sh
python -S tools/stock_still.py restore input.gpr --sample-bits 14 \
  --sdk build-runtime/source/app/gpr_tools/gpr_tools \
  --checkpoint models/stock-q3-rggb14-cleanup/selected.pt \
  --native-executable build-runtime/source/app/stock_metal/gpr_reconstruct \
  --native-artifact /new/coreml-f32 --native-backend coreml --native-units cpu \
  --output restored.dng
```

For Metal, choose the Metal artifact, `mpsgraph` or `hybrid`, and add
`--native-kernels build-runtime/source/app/stock_metal/gpr_stock.metallib`.
Exported artifacts are explicit external files; no model is downloaded or
selected automatically. The original camera/SDK dependency boundaries remain.

[FP32/Metal protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/native-protocol.json),
[export contracts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/native-exports.json),
[twelve-case results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/native-native-check.json)
and [driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/run-native.py) bind the successful
checks. [FP16 protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/native-f16-protocol.json),
[export](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/native-f16-exports.json),
[failed-bound results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/native-f16-native-check.json)
and [driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/run-native-fp16.py) preserve that
separate experiment. Source/model/native hashes and exact command lines are
in the receipts. [All seven hosted jobs](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-native-receipts/hosted-c47fdf1.json)
passed at `c47fdf1`, including the generic native backend contracts on Mac;
the real-checkpoint measurements above are separate local evidence.
