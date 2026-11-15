# Exact older 8K checkpoint: direct stock transfer and matched control

The exact older selected checkpoint is available and was tested directly,
rather than assuming that a similar architecture reproduces its behavior.
On the supplied GP011689 count reference with current stock-Q3 lows, its
active PSNR is **5.36022 dB below current bicubic** and **5.44305 dB below the
supplied stock BIBO diagnostic**. All four RAW regional metrics regress. No
old checkpoint is selected as a stock replacement.

## Fixed transfer test

Checkpoint SHA-256
`791628778f74b13542677edeb5de7934b88d274531a8ec1755d8cf3d6388fb07`
matches the frozen PR #95 registry. The model/runtime source is taken directly
from revision `8be29eee57b4637865b572f18ab9e3f4edaa2236` and hash-bound.
The saved model config is coordinate/detail/preclean PixelShuffle,
width 96/depth 6, residual scale 0.3. Strict state loading uses the exact
checkpoint; weights, output strength and preprocessing are not tuned.

The old registered tile 512/overlap 32/global-coordinate settings process
exactly the same decoded stock-Q3 low counts as the current supplied model.
The output has the expected dimensions/count domain and can be packaged with
the same supported high calibration. Numeric failure prevents promotion;
packaging support alone is not evidence of model quality.

This is a transfer test for one current source/input combination. The older
training contract uses fused q4/t2 detail-sidecar lows and Gaussian-area
simulation; this current test uses stock Q3 and same-site area averaging.
Scene/domain differences also remain. The result does not isolate codec,
degradation, or generalization as the cause of the loss.

## Matched historical control

To check the checkpoint/runtime reconstruction, one archived old matched
input/target pair (GP017346) is also processed. Its low-input bytes agree with
the SHA recorded in the old benchmark. The original selected-model record is
**60.30148693 dB**; the current saved prediction measures **60.30148779 dB**,
a difference of **0.000000854 dB**. The archived original prediction file is
unavailable, so this is an observed metric reproduction, not byte-identity
with that historical output.

The first control analysis omitted CFA labels in its diagnostic metadata and
stopped while computing per-site metrics after inference had completed. A
separate metrics driver reuses the saved prediction, supplies the required
site labels and records its own hash. It does not rerun inference or modify
weights. Global count distortion is computed against the archived RAW count
reference; original GPRs are not freshly decoded and no per-frame calibration
claim is made for this control.

[Transfer protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-old-sr-transfer-receipts/protocol.json),
[transfer result](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-old-sr-transfer-receipts/summary.json),
[matched control protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-old-sr-transfer-receipts/legacy-control-protocol.json),
[control result](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-old-sr-transfer-receipts/legacy-control.json) and
[manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-old-sr-transfer-receipts/manifest.json) preserve identities,
metrics, limitations and source. The old runtime modules are retained as
analysis evidence, not added as an installed production backend. Large
outputs and the old checkpoint remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_old_selected_transfer_20261030`.

The [new recipe comparison](stock-sr-recipe-comparison.md),
[signal-conditioning experiment](stock-sr-signal-conditioning.md) and
[actual supplied-camera checks](stock-cnn-gopro.md) are separate studies.
Successful old-profile results are not relabelled as stock-profile approval.
