# Related-checkpoint weight interpolation on stock inputs

The older selected recipe interpolated related checkpoint weights. This
follow-up tests that specific operation on the current stock SR family;
none of its ten settings fixes the Nikon shadow regressions. Supplied models,
production inference and existing quality scopes remain unchanged.

## Fixed family and controls

The supplied 2x SR checkpoint is the common ancestor of the SR-only
[matched-training](stock-cnn-joint.md) checkpoints at updates 200 and 400.
For each endpoint, five fixed parameter weights are evaluated: 0.125, 0.25,
0.5, 0.75 and 1.0. Each floating tensor is computed as
`((1-alpha)*base.float64 + alpha*endpoint.float64).to(original_dtype)`.
Batch-normalization running statistics and all nonfloating buffers are
identical and remain exact. Architecture, residual scale, normalization and
inference metadata come from the supplied checkpoint. This does not blend
predictions or sweep one model's output strength.

The protocol is saved before evaluation. All settings use the same 320
source-grouped validation patch cores, actual stock-decoded inputs, extended
256-plane context and calibrated tone/WB proxy as the parent study. Bicubic,
supplied-model and both endpoint RAW scores must reproduce exactly. A
candidate must preserve all four RAW regions in every source group against
both bicubic and supplied SR, preserve each group's tone proxy, and improve
pooled RAW fidelity. Every patch qualifier must then pass all ten complete
validation images and actual calibrated rendering before final evaluation.
No reference pixels change thresholds or define the parameter family.

## Results

Gain is pooled RAW PSNR versus supplied SR.

| Endpoint update | Alpha 0.125 | Alpha 0.25 | Alpha 0.5 | Alpha 0.75 | Alpha 1 |
|---|---:|---:|---:|---:|---:|
| 200 | +0.00358 | +0.00748 | +0.01605 | +0.02490 | +0.03270 |
| 400 | +0.00651 | +0.01390 | +0.03127 | +0.05157 | +0.07369 |

Every setting passes the tone-proxy guard and improves pooled RAW fidelity,
but every setting regresses Nikon shadows. Lower parameter weights reduce
the aggregate change without removing the regional failure. No candidate
qualifies, so whole-image, rendered, final-reference, native, Pi and temporal
quality evaluations are not run for this family. This rejects these related
endpoints and fixed interpolation weights, not interpolation generally or
the earlier model's historical result.

An independent audit compares 1,530 saved parameter tensors against NumPy
float64 interpolation, reruns all twelve settings including controls, and
reproduces every prediction hash and RAW/proxy score exactly. It independently
reconstructs guard decisions and confirms each candidate's Nikon shadow
failure. Existing reused validation/pretraining overlap and compressed-camera
reference limitations remain; these are not blind or sensor-truth results.

## Evidence

[Summary](stock-cnn-interpolation-receipts/summary.json),
[offline archive](stock-cnn-interpolation-receipts/evidence.tar.gz) and
[member hashes](stock-cnn-interpolation-receipts/manifest.json) preserve the
protocol, evaluator, producer/audit drivers, full results and logs. The ten
archive members are checked byte for byte after packaging. Checkpoints and
source media stay external at
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_cnn_weight_interpolation_20261031`.

This closes the missing stock weight-interpolation comparison. It does not
close useful broad 2x detail, rendered/temporal quality or restored-tier
approval. Better pooled scores alone remain insufficient for model selection.
