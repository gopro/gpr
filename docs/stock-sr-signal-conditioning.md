# Stock SR: calibration audit and signal-dependent residual training

This experiment tests a specific response to measured shadow overshoot:
calibrated input features and a signal-dependent residual gate during training.
It uses the existing BIBO backbone, audited stock pairs and unchanged regional
quality criteria. Supplied checkpoints and production runtime remain unchanged.

## Calibration and count audit

All eight training and four validation sources have the same recorded Bayer
black levels (1008 in all four sites) and white level (15892). A differing
metadata pedestal across these sources therefore does not explain the observed
failure. The audit excludes final-source pixels and metadata. It binds each
source, metadata-derived values, patch count ranges and codec residual metrics.

The signed codec error in low-resolution shadow cores varies by source:
training-source means range approximately -2.75 to +0.26 counts; validation
includes a +11.85-count mean on ForestFire_010. Individual training shadow MSEs
range from 17.51 to 1744.31 counts squared. These are source-specific sampled
core regions, not full-image model-quality measures. They show that a common
black level does not imply a common error distribution.

Current models divide physical counts by 16383 without subtracting metadata
black. The alternative feature input is `(counts - 1008) / (15892 - 1008)`.
Values below black remain negative: no signal clipping changes the observed
counts. Output base, loss and quantization stay in the original count domain.
This is a feature-conditioning experiment, not a correction of image metadata.

## Fixed four-case design

Two binary factors give raw/calibrated feature input, with/without a residual
gate. All cases use the same BIBO architecture, initial weights, sampling
sequence, optimizer, source-wide region weights and raw-domain residual scale
0.01. The gate is `s / (s + 0.02)`, where
`s = max((bicubic_counts - 1008) / (15892 - 1008), 0)`.
It uses only the model's bicubic input and fixed audited calibration; it does
not inspect a reference image. Gating participates in training and inference.
The fixed 0.02 is an exploratory signal scale, not a measured sensor-noise model.

Each case runs 3,000 AdamW updates at learning rate 0.0003, batch eight, seed
20261103. Every batch has one patch from each training source, using the same
random indices across cases. Snapshots at 1000/2000/3000 each have eight fixed
output blends, for 96 patch configurations. Zero heads must reproduce bicubic
counts exactly at initialization. The original independent patch baseline and
cached-mask evaluator must agree exactly.

All patch-qualified settings, with unchanged pooled/per-group regional
nonregression and strict pooled improvement, proceed to four complete
validation images. There is no top-three shortlist. Complete-image selection
requires no active/shadow/highlight/detail RMSE increase on any image, plus
strict pooled active improvement. All sources are reused development data;
this is not an unseen test. A selected configuration must be frozen before
any separate historical-reference evaluation.

## Scope and reproduction

This is a one-seed analysis-only experiment for the audited calibration/domain,
not automatic support for other camera black levels, CFA phases or bit depths.
No native or Pi runtime adoption follows from training support. Architecture,
checkpoint, source/array, code and protocol hashes accompany the numerical
receipts; large arrays/photos/checkpoints remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_signal_conditioning_20261030`.
The separate audit is under `stock_sr_calibration_20261030`.

Real rendered detail, temporal behavior, broader source coverage and useful
quality improvements remain required. Earlier recipe/strength failures are
retained; a better patch score is not a complete-image qualification.

## Measured result

Four 316,800-parameter cases complete 12,000 updates in 182.86 seconds.
All twelve saved snapshots are finite. Thirty-one patch-qualified settings
proceed to 124 full-image inferences (161.75 seconds); 27 pass every regional
guard. Names encode normalized features (`n`) and signal gate (`g`).

| Case | Patch-qualified settings | Full-image qualifiers | Best full gain, dB |
|---|---:|---:|---:|
| n0g0 | 7 | 4 | +0.051034 |
| n0g1 | 7 | 7 | +0.309849 |
| n1g0 | 10 | 9 | +0.096122 |
| n1g1 | 7 | 7 | +0.316799 |

The gate explains most of the validation improvement; feature centering adds
a smaller gain. The combined case at step 2000/strength 0.5 is frozen before
historical-reference evaluation. All four reference metadata packets match
the declared black1008/white15892 domain.

The frozen model fails historical regional checks on both CPU and MPS.
Z8Z_0001 improves active PSNR by about 0.619 dB; Z8Z_0067 improves overall but
regresses shadows; Z8Z_5323 and Z8Z_6693 regress active error (about -0.0366
and -0.0209 dB). Six of 32 regional comparisons fail. Pooled gain remains
positive (+0.04142 dB), which does not override those failures. The candidate
is not supplied or promoted, and no strength is selected using these final
results. This exposes generalization rather than a CPU/MPS-only discrepancy.

[Training protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/protocol.json),
[all patch results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/training.json),
[complete-image family](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/full-protocol.json),
[selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/full-selection.json),
[freeze](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/frozen.json),
[final protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/final-protocol.json),
[CPU/MPS results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/final-summary.json) and
[snapshot audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/snapshot-audit.json) retain the result.
The calibration [audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/calibration-audit.json) and
[protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-signal-receipts/calibration-protocol.json) exclude final
pixels and metadata from the initial audit.

The [actual GoPro-source audit](stock-sr-mission-sources.md) separately finds
black16/white16383 in supplied camera files, so this fixed Nikon-calibration
variant is not claimed as a GoPro model. Current supplied 1x/2x checkpoints
remain unchanged and have their own positive [GoPro checks](stock-cnn-gopro.md).
Broader scene/codec-domain training, robust RGB/RAW supervision and controlled
re-evaluation are more useful next work than final-image retuning.
