# Supplied 2x diagnostic: native checkpoint qualification

The optional [source-wide 2x diagnostic](stock-sr-source-regions.md) now passes
checkpoint-specific standalone FP32 Core ML, MPSGraph and custom Metal hybrid
checks. The exact checkpoint is
`f7bda0449b94be4ebd3617297b9dee04506b00e38788dba2d0b04eb71fdaa2f4`.
No training, strength selection or checkpoint changes occur in this follow-up.
The separate 1x model and production encoder/capture code are unchanged.

## Execution and count agreement

All twelve combinations of two supplied 4096x3072 stock-Q3 captures and six
native configurations produce 8192x6144 editable DNGs successfully. Supported
calibration, independently read TIFF pixels and decoded input hashes match the
recorded contracts. Every command runs the media frontend with `python -S`;
Torch, NumPy and Core ML Tools are absent from that runtime control path.
The external evaluation parent uses NumPy/tifffile for independent comparisons.

FP32 Core ML retains its preregistered one-count bound. MPSGraph and hybrid use
the existing folded FP16 artifact and retain their 32-count bound; observed
maximum error is one count across all twelve cases. This observation does not
replace the broader Metal bound. Core ML unit flags select permitted policies,
not evidence that individual operations executed on a particular device.

## Full-reference quality after conversion

All six configurations also process the four historical Nikon full references:
24 complete native restores, with 96 active/shadow/highlight/detail comparisons.
Every comparison passes the unchanged nonregression guard. Native counts differ
from the corresponding frozen Torch MPS model output by at most one count.

| Native configuration | Full-reference cases | Maximum count difference | Minimum regional gain, dB |
|---|---:|---:|---:|
| coreml / cpu | 4 | 1 | 0.00046088 |
| coreml / gpu | 4 | 1 | 0.00046088 |
| coreml / ane | 4 | 1 | 0.00046088 |
| coreml / all | 4 | 1 | 0.00046088 |
| mpsgraph / all | 4 | 1 | 0.00046140 |
| hybrid / all | 4 | 1 | 0.00046144 |

The common quality baseline is the frozen Torch MPS bicubic result from the
published selection study, on the identical Q3 low-resolution input; a new
native bicubic baseline is not substituted. Reference RAW bytes, input counts,
checkpoint, calibration, native executable and kernels are bound by the
protocols/receipts. No reference scores select new weights or strengths.
These four references have been used in prior research and are not a newly
blind test set.

The original pooled improvement is only +0.00729 dB. Conversion preserves the
narrow numeric result; it does not establish useful recovered detail, rendered
look, temporal quality, other camera domains or restored compression tiers.
`quality_approval=false` remains in force. Pi execution remains offline at
86.29–99.89 seconds per frame; native Mac checks do not change that measurement
or close the recording deadline gate. Individual Mac timings are diagnostic
costs, with caches not flushed, and are not sustained throughput comparisons.

## Reproduction and evidence

Export with optional build-time dependencies; use fresh external directories:

```sh
python3 tools/stock_coreml.py models/stock-q3-rggb14-sr-diagnostic/selected.pt \
  --output-dir /tmp/stock-sr-coreml-f32 --precision float32 --max-plane 512
python3 - <<'EXPORT'
from pathlib import Path
import sys
sys.path.insert(0, 'tools')
from stock_metal import export
export(Path('models/stock-q3-rggb14-sr-diagnostic/selected.pt'),
       Path('/tmp/stock-sr-metal-f16'), max_plane=512)
EXPORT
python3 -S tools/stock_still.py restore input.gpr --sample-bits 14 \
  --sdk build-runtime/source/app/gpr_tools/gpr_tools \
  --checkpoint models/stock-q3-rggb14-sr-diagnostic/selected.pt --scale 2 \
  --native-executable build-runtime/source/app/stock_metal/gpr_reconstruct \
  --native-artifact /tmp/stock-sr-coreml-f32 --native-backend coreml \
  --native-units all --native-threads 4 --tile 256 --halo 64 --output native-2x.dng
```

For MPSGraph/hybrid, select the corresponding `--native-backend`, the exported
Metal directory and `--native-kernels` pointing to `gpr_stock.metallib` adjacent
to the native executable. See the exact archived commands for each case.

[Exports](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/exports.json),
[execution protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/protocol.json),
[twelve-case receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/native-check.json),
[quality protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/quality-protocol.json),
[24-reference receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/quality.json),
[execution driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/run-native.py) and
[quality driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-native-receipts/run-quality.py) retain exact evidence.
Drivers record original installation/data paths; copy them to a fresh output
root and adapt paths when reproducing, regenerating bindings rather than
replacing historical receipts. Native packages, source images and full DNG
outputs remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_source_regions_20261030/all-qualified/native`.
They are not installed camera SDK dependencies or automatic downloads.
