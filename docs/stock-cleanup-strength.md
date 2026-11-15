# Stock-Q3 1x cleanup with validated residual strength

An optional fresh 1x candidate now passes the declared pooled/per-group
validation guard and the four-reference full-resolution numeric guard. It
improves active-area PSNR by **0.328–0.854 dB**, with lower shadow, highlight
and detail MSE on every reference. The pooled active-area gain is **0.639 dB**.
This closes that specific 1x numeric test; broader-camera, rendered visual,
temporal, restored-tier and 2x quality approval remain open.

The [checkpoint and model card](../models/stock-q3-rggb14-cleanup/README.md)
are supplied separately in the repository. Nothing selects or downloads it
automatically, and camera/SDK packages do not install weights or gain a Torch
dependency. The checkpoint retains `quality_approval=false` to avoid relabeling
a scoped numeric result as general production approval.

## Training and validation-only selection

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/protocol.json) was written before
fresh training. It reuses the audited Q3/RGGB14 1x dataset: eight training
sources in four declared groups, four validation sources in two groups and
four historical final references. There are 512 training and 128 validation
128x128 Bayer-plane patches. Final references contribute no patch-array pixels.
The [fresh split audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/split-audit.json) checks
source-byte uniqueness, group separation and disjoint reference RAW hashes.
Standalone evaluator receipts retain a generic split-unknown disclaimer;
this companion audit supplies the recipe-specific separation evidence.
This establishes exact identity separation, not perceptual near-duplicate
exclusion. The final references have appeared in historical studies and are
not a newly unseen collection.

Fresh seeded `F_ane_no_sr` weights use width 16/depth 3, normalization 16383,
a zero residual head, training residual scale 0.01, AdamW, learning rate
0.0003, weight decay 0.0001, batch eight and seed 20261029. The existing
shadow-weight-three loss runs 3000 steps on MPS. Training takes **44.22 s**.
The lowest rounded-count overall validation MSE selects a *provisional*
checkpoint at step **1300**, MSE **1447.5734 → 1244.7675**. This preliminary
selection does not apply the regional guard and does not approve that model.

The predefined strength family multiplies its existing residual scale by
`1, 1/2, 1/4, …, 1/65536`. Selection requires strictly lower rounded validation
MSE than identity and **no increase** in overall/shadow/highlight/detail MSE,
both pooled and in each validation group, using the unchanged `region_guard`.
No tolerance is added. Counts are quantized exactly as deployed. Masks remain
reference-only patch percentiles and same-site gradients, as in the earlier
experiment. Strength is applied before clamping, using the normal checkpoint
formula; this is not a post-clamp image blend or a new model architecture.

Nine of 17 strengths qualify. **Half strength** wins among eligible candidates:
residual scale **0.005**, validation MSE **1285.1725**, gain **0.517 dB**.
Full strength fails the regional guard. The selector independently reproduces
the training run's identity and full-strength MSE exactly. Training history,
all strengths, rejected regions and selection bindings are retained in the
[training](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/training.json) and
[selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/selection.json) receipts.

## Frozen full-resolution evaluation

The selected checkpoint SHA-256 is
`9de03f8203f4cb1bb264087cd922b04c1b0e1169684a064d0aeb3a2a6c97f48a`.
The [freeze receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/frozen.json) binds that
file, selection/protocol, evaluator and SDK before final evaluation. Its
`final_evaluation_started=false` records the state when written; subsequent
completed receipts prove evaluation occurred afterward. Final-image results
do not select another strength or replace the checkpoint.

Each lossless sensor DNG is independently decoded, compared with its frozen
RAW reference, and evaluated against the same stock-Q3 decoded input counts
used in the earlier identity study. A fresh SDK decode must match those counts;
independent TIFF reads verify reconstructed pixels and exact supported
calibration. The [input bindings](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/native-input-bindings.json)
record identities, including the changed 0001 native benchmark manifest whose
source and decoded Q3 pixels still match the prior study exactly.

| Historical reference | Active gain, dB | Shadow gain, dB | Highlight gain, dB | Detail gain, dB |
|---|---:|---:|---:|---:|
| Z8Z_0001 | +0.328 | +1.137 | +0.159 | +0.157 |
| Z8Z_0067 | +0.353 | +1.218 | +0.267 | +0.192 |
| Z8Z_5323 | +0.854 | +1.009 | +0.686 | +0.457 |
| Z8Z_6693 | +0.540 | +0.909 | +0.270 | +0.101 |

Every listed scene/region improves; there is no aggregate gain hiding a failed
scene or region. Pooled active PSNR uses sample-count-weighted MSE reconstructed
from reported RMSE, with peak 16383; dB values are not averaged. The full-image
reference masks differ from patch-validation masks, and both are checked.
Diagnostic MPS inference on these 45.7 MP inputs takes 2.167 s on the first
invocation and 0.710–0.757 s afterward. These are tiled inference measurements,
not complete camera workflows, warmed first-use latency or temporal evidence.

The [summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/final-summary.json) and individual
[0001](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/final-0001.json),
[0067](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/final-0067.json),
[5323](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/final-5323.json) and
[6693](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/final-6693.json) receipts preserve metrics,
source/output hashes, metadata, software identity and timing boundaries.

## Pi portability and cost

The identical frozen checkpoint restores two supplied 4096x3072 stock-Q3
frames through the normal CPU still frontend on Pi 5, four Torch threads,
tile 256/halo 64. Independent TIFF reads match output hashes and verify
12 MP RGGB14 counts and exact supported calibration. Inference takes
**95.73 / 95.96 s**; complete per-frame decode/reconstruct/editable workflows
take **96.27 / 96.44 s**. This is offline reconstruction. The sources are
still-derived replays, not new lossless sensor references or additional
cross-camera quality approval. [Pi receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/pi5.json).

Mac MPS and Pi CPU decode identical input counts. Their reconstructed counts
differ by at most **one**: 197/177 differing samples out of 12,582,912 each;
supported calibration remains exact. Backend results are not bit-identical.
MPS inference on these 12 MP frames takes 0.407/0.401 s in this check.
[Cross-host receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/cross-host.json).
The later [checkpoint-specific native checks](stock-cleanup-native.md) pass
FP32 Core ML and MPSGraph/hybrid Metal on the same two 12 MP inputs through
standard-library media commands. FP16 Core ML exceeds its predefined precision
budget and remains rejected; generic backend tests are separate evidence.

## Use and reproduction

Use the model-card command with an explicit checkpoint and Q3/RGGB14 source.
Other presets, CFA/count domains and 2x inference do not inherit this study.
The ordinary codec/default model policy is unchanged.
The [repository-asset check](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/repository-asset.json)
loads the supplied file through the normal adapter and verifies finite,
bounded, nonidentity 1x output; that smoke check is separate from quality scores.

The existing `tools/train_stock_sr.py train` command reproduces the provisional
training schedule with the protocol parameters and without `--guard-regions`.
Then the [selection driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/run-selection.py)
applies the unchanged strict region guard across the preregistered strength
family. Freeze the selected hash before running the
[final evaluator driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/run-final.py).
The [Pi driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/run-pi.py) and
[cross-host check](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-strength-receipts/run-cross-host.py) bind the
portability measurements. Driver paths identify the measured installation;
choose a fresh output root and adjust paths when reproducing elsewhere.

Photos, patch arrays, full DNG outputs and provisional training weights remain
external under `/Volumes/OWC_8TB/gpr_work/artifacts/stock_cleanup_strength_20261030`.
The selected 1.3 MB research checkpoint is the explicit repository asset.
