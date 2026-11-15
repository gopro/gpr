# Source-wide 2x loss and complete-image selection

A fresh stock-Q3/RGGB14 2x checkpoint now passes the declared patch guards,
all four complete validation-image guards, and all four historical final
references on both Torch MPS and CPU. Its measured benefit is small:
**0.00046–0.06133 dB** active PSNR per final image, **0.00729 dB pooled**.
The exact [diagnostic checkpoint](../models/stock-q3-rggb14-sr-diagnostic/README.md)
is supplied explicitly. `quality_approval=false` remains; this is not restored-tier,
rendered/temporal, broader-camera or useful recovered-detail approval.

## Why change the loss

The earlier [patch/full-frame diagnostic](stock-sr-relative.md) showed that
patch guards can pass while complete validation images regress. The masks also
have different definitions: the recipe uses per-patch 10/90 percentiles and
backward same-site gradients, while final metrics use whole active-area
percentiles and a forward same-site gradient.

On the **training cores only**, just 36.72% of locally labelled shadow samples
and 35.49% of locally labelled highlight samples also lie in the source-wide
regions. These are different masks, not evidence that either percentile
calculation is incorrect. Source-wide training aligns the loss with the
existing full-image metrics; the original patch guards remain unchanged.

## Audited training-source weights

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/protocol.json) precedes weight
construction and training. Only the eight training sources are decoded to
construct weights. Validation/final sources are excluded. Source bytes and
independent reference RAW hashes match the audited dataset; all 512 target
core coordinates match the source counts exactly.

For each source, define full active/shadow/highlight/detail masks exactly as
`benchmark_stills.quality`. Its clipped, tiled bicubic reconstruction uses the
same actual stock-Q3 decoded low input as the model. Regional errors agree with
the existing independent evaluator. A training pixel receives each applicable
region's weight `N / (4 * n_r * max(MSE_r, 1))`, summed across the four regions.
`N` is the complete source pixel count, `n_r` its region population and `MSE_r`
the source's bicubic error. Pixels outside active receive zero weight.

This approximates an equal mean of source-wide regional error ratios using
sampled cores. Sampling underrepresents borders and does not constitute full
image training. An independent coordinate/forward-neighbor reconstruction
matches stored masks/weights exactly on a patch from each training source;
direct regional-ratio and weighted-pixel loss agree within the predefined
1e-6 relative check. Nontraining weight rows remain zero.

[Weight audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/weight-audit.json),
[weight binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/weight-binding.json),
[independent loss audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/loss-audit.json) and
[builder](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-weights.py) retain these checks.
Weight construction takes 15.16 s; arrays/photos remain external.

## Fixed patch and full-image selection

Fresh `F_ane` training uses width 16/depth 3, seed 20261101, normalization 16383,
residual scale 0.01, batch eight, 3000 steps, learning rate 0.0003 and checks
every 300 steps. It takes 47.28 s. The
[derived recipe](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-training.py) is archived;
production training/runtime code is unchanged.

The predefined ten snapshots and 17 strengths produce 170 patch candidates.
Twenty-two pass the unchanged exact pooled/per-group overall/shadow/highlight/
detail guards and improve overall MSE. A first family evaluates only the best
patch strength per snapshot: three candidates across four full validation
images. Two fail full-image shadows; a near-bicubic 1/1024-strength candidate
passes and subsequently passes the historical MPS/CPU reference checks. Its
pooled final gain is only 0.000229 dB. It is not the supplied asset.

A separately recorded follow-up fixes **all 22 patch-qualified combinations**
before evaluating all four complete validation sources. No final pixels select
strength. All 88 full-image inferences complete; thirteen candidates pass.
The lowest sample-weighted active MSE selects **step 3000 at 1/32 strength**,
residual scale **0.0003125**, full-validation gain **0.02782 dB**. Per-image
regional nonregression and strict pooled improvement are unchanged.

The [family protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-family.json),
[full protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-full-protocol.json),
[full selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-full-selection.json) and
[freeze](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-frozen.json) bind this choice before
that candidate's final evaluation. Historical references had already appeared
in previous studies, including the preliminary family; they are not newly
unseen or a claim of an untouched blind test set.

## Historical final references

| Reference | Active gain, dB | Shadow gain, dB | Highlight gain, dB | Detail gain, dB |
|---|---:|---:|---:|---:|
| Z8Z_0001 | +0.061331 | +0.033082 | +0.072718 | +0.069479 |
| Z8Z_0067 | +0.034041 | +0.043177 | +0.045260 | +0.043036 |
| Z8Z_5323 | +0.000954 | +0.006601 | +0.008090 | +0.002142 |
| Z8Z_6693 | +0.000461 | +0.004901 | +0.006503 | +0.001526 |

All sixteen MPS regional comparisons improve. CPU independently passes the
same sixteen comparisons against its own bicubic baseline. Sample-weighted
active MSE gives pooled gains **0.00729163 dB MPS / 0.00729159 dB CPU**;
these are not averages of per-image dB. Supported scaled calibration and
editable output pixels are independently checked; input/reference hashes match
the prior studies. Neither the stronger earlier failures nor the weaker first
family is relabelled as this result.

## Pi execution and supplied asset

The exact frozen checkpoint is supplied as
`models/stock-q3-rggb14-sr-diagnostic/selected.pt`, SHA-256
`f7bda0449b94be4ebd3617297b9dee04506b00e38788dba2d0b04eb71fdaa2f4`.
It loads through the normal adapter, with finite, bounded, nonidentity 2x output.
No automatic selection/download or camera/SDK ML dependency is added.

Both supplied 4096x3072 stock-Q3 replay inputs produce 8192x6144 editable RAW
on Pi CPU and Mac MPS. Independent TIFF reads verify output counts and exact
supported scaled calibration. CPU/MPS differ by at most **one** count:
237/219 differing samples out of 50,331,648 each. The one-count bound is fixed
before this comparison. These supplied still-derived inputs have no lossless
GoPro high-resolution reference, so execution agreement is not a GoPro quality
score.

Pi inference takes 99.89/86.29 s; whole workflows 100.82/87.19 s. Mac MPS
inference takes 0.481/0.286 s in the checks. These are individual offline
execution costs, not sustained throughput or controlled speedup comparisons.
[Native artifacts for this checkpoint](stock-sr-native.md) now pass 12 execution
and 24 full-reference checks through standalone Core ML/Metal media commands. The separate 1x
candidate, production codec/capture source and strict Pi recording timing gate
remain unchanged.

## Receipts and reproduction

[Summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/summary.json),
[training](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/training.json),
[training binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/training-binding.json),
[patch selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/patch-selection.json),
[patch selector](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-patch-selection.py),
[final MPS summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-final-summary.json),
[CPU protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-cpu-protocol.json),
[CPU summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-cpu-final-summary.json),
[final driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-all-final.py),
[CPU driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-all-cpu-final.py),
[split audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/split-audit.json) and
[asset check](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/repository-asset.json) bind the result.
Individual validation/final/CPU benchmark files in the receipt directory retain
exact counts/metrics, source and tool identities; source separation excludes
exact duplicates, not untested perceptual near-duplicates.

[Pi execution](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-pi.json),
[MPS execution](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-mps-portability.json),
[cross-host protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-cross-host-protocol.json),
[comparison](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/all-cross-host.json),
[Pi driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-all-pi.py) and
[comparison driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-source-region-receipts/run-all-cross-host.py) retain
portability evidence. The `three-*` files preserve the weaker preliminary
family, metadata-only packaging and its independent checks; only the later
exact frozen checkpoint is supplied.

Copy archived drivers/protocols to a fresh external output root and adapt the
recorded installation/data paths when reproducing. Regenerate bindings when
paths/code/data change; do not overwrite historical receipts. Source photos,
patch/weight arrays, earlier checkpoints and full outputs remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_source_regions_20261030`.
No visual/temporal, broader-camera or restored-tier approval follows from this
narrow numeric improvement.
