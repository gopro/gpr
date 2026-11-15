# Stock SR: pre-cleaning, detail features and structure-loss comparison

This fixed exploratory comparison adapts components of the older selected
Mission 1 SR recipe to actual stock-Q3 training pairs. It tests eight variants
of a small low-resolution PixelShuffle adapter, rather than making additional
strength changes to the supplied BIBO checkpoint. No production architecture,
model file or native/backend behavior changes.

The completed all-qualified follow-up finds ten passing settings out of 48,
but its best +0.00735 dB full-validation gain is below the current BIBO model
(+0.02782 dB) on the identical inputs. This small adapted recipe is not selected
as a quality replacement. The initial shortlist failure and its correction
remain documented below.

## Fixed experiment

The protocol precedes training and binds source arrays, audited training-only
region weights, code and candidate family. The existing dataset has eight
training sources/512 patches and four validation sources/128 patches. No final
reference pixels occur in either patch array. Every step samples one patch
from each training source; the random index sequence is identical across cases.

Three binary factors produce eight cases:

- **Pre-cleaning:** a three-convolution low-resolution correction before SR,
  with auxiliary supervision against rounded same-site 2x2 averages of training
  high-resolution targets. The factor includes this auxiliary loss at 0.05;
  this is not an architecture-only pre-cleaning ablation.
- **Detail:** append four deterministic same-site high-pass channels, using the
  historical binomial kernel. This increases the input parameter count.
- **Structure loss:** add same-site gradient and Laplacian error ratios at
  weights 0.4/0.05. Fixed bicubic training-patch error energies normalize them;
  no validation/final pixels determine loss denominators.

All cases use width 16, seed 20261102, AdamW learning rate 0.0003, batch eight,
3,000 updates and snapshots at 1,000/2,000/3,000. SR residual heads and preclean
output heads initialize to zero. Each initial model reproduces bicubic exactly
on validation counts, and the baseline matches the earlier independent patch
receipt. Training checks finite losses. Source-wide region MSE weights are
those from the audited prior experiment; only training indices access them.

The architecture uses low-resolution standard/dilated convolution branches and
PixelShuffle adapted from the historical adapter. The base is changed from
historical bilinear to the current bicubic baseline. Parameters range from
12,784 to 17,428; this is substantially smaller than the old selected width-96
model and is not the current BIBO architecture. Coordinates, detail-phase loss,
checkpoint interpolation and larger-model comparisons remain separate work.

## Selection and interpretation

Each snapshot has eight predefined output-blend strengths, yielding 192 patch
configurations. Blending is between the clipped model output and bicubic; this
is neither parameter interpolation nor a change to trained weights. Exact
pooled/per-source-group overall/shadow/highlight/detail nonregression and
strict overall improvement remain required. Cached mask arithmetic is checked
against the existing independent region evaluator.

Before complete-image evaluation, take the lowest-MSE three qualifying patch
configurations per case, as fixed in the training protocol. Eighteen candidates
qualify for this stage. This bounded shortlist can miss a weaker patch candidate
that would do better on full images; it is not an exhaustive full-image search.

Every shortlisted candidate runs on all four complete validation images, using
hash-bound SDK references and actual decoded stock lows. Bicubic pixel hashes
and full metrics must reproduce prior independent benchmarks. Selection again
requires no active/shadow/highlight/detail RMSE increase on any image, plus
strict pooled active improvement. Historical final references do not select
architecture, loss, strength or snapshot in this experiment.

The one-seed design and reused small validation set limit causal/generalization
claims. Detail changes parameter count; preclean includes auxiliary supervision;
structure objectives need not improve a pure MSE score. These effects should
not be presented as universal gains or a reproduction of the entire old recipe.

## Evidence and next requirements

Scripts, protocols, numerical results and manifests accompany this report.
Arrays, checkpoints and photos remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_recipe_comparison_20261030`.
A passing selection is still validation only. Historical-reference quality,
CPU/Pi/native execution, editable output integration, calibrated rendered
appearance, useful detail and temporal consistency need their own evidence
before changing the supplied model. Earlier failures remain recorded.

## Measured outcome

All eight training cases finish in 188.77 seconds; the initial 72 full-image
inferences finish in 89.52 seconds. The initial shortlist selects no model: all eighteen
shortlisted candidates regress at least one complete-image regional metric.
The complete results, including higher aggregate gains that fail shadows,
remain in the receipts. No historical final-reference evaluation follows this initial shortlist.

Case names encode preclean (`p`), detail (`d`) and structure loss (`s`).

| Case | Parameters | Best eligible patch gain, dB | Full-image candidates | Outcome |
|---|---:|---:|---:|---|
| p0d0s0 | 12784 | +0.022976 | 3 | All rejected |
| p0d0s1 | 12784 | +0.022903 | 3 | All rejected |
| p0d1s0 | 13936 | +0.126368 | 3 | All rejected |
| p0d1s1 | 13936 | +0.110352 | 3 | All rejected |
| p1d0s0 | 16276 | — | 0 | No patch qualifier |
| p1d0s1 | 16276 | +0.030574 | 3 | All rejected |
| p1d1s0 | 17428 | — | 0 | No patch qualifier |
| p1d1s1 | 17428 | +0.157951 | 3 | All rejected |

The plain adapter's best eligible patch gain is +0.02298 dB; explicit detail
raises it to +0.12637 dB. Adding preclean and structure produces +0.15795 dB
on patches. These are separate patch results, not passing full-image gains.
Both preclean cases without structure loss have no patch-qualified candidate.
This bounds the evidence: the small adapted recipe fails to qualify; it does
not establish that larger historical models or every recipe variant fail.

A subsequent signed-residual diagnostic below measures why stronger aggregate
scores still fail shadows. Further work should distinguish regional amplitude
from spatial/frequency error placement, rather than assuming a larger network
or global strength alone resolves the tradeoff. Current supplied checkpoints and production source remain
unchanged. The separately frozen intensity gate has its own modest passing
[numeric result](stock-cnn-tactics.md#frozen-gate-historical-reference-and-cpu-follow-up);
that result does not rescue this rejected architecture family.

[Training protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/protocol.json),
[training and all patch candidates](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/training.json),
[fixed full-image family](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/full-protocol.json),
[complete-image results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/full-selection.json) and
[receipt manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/manifest.json) bind the measurements.

## Signed-residual diagnostic

For the rejected `p1d1s1` step-2000/strength-0.25 candidate on validation image
3, a fresh replay reproduces the exact recorded output and baseline hashes.
Define baseline error E and the measured count correction R. The exact change
in MSE is `2 mean(E R) + mean(R²)`; independently computed squared errors agree.

| Region | Cross contribution `2 mean(E R)` | Residual energy `mean(R²)` | MSE change |
|---|---:|---:|---:|
| Active | -55.453663 | 5.097256 | -50.356407 |
| Shadows | -0.211554 | 0.534679 | +0.323125 |
| Highlights | -532.382475 | 43.362352 | -489.020123 |
| Detail | -460.489189 | 38.692053 | -421.797136 |

The shadow correction has a helpful cross term but too much energy: it
overshoots, rather than pointing entirely in the wrong direction. The optimal
unconstrained multiplier of this measured residual is approximately 0.198 in
shadows versus 5.44 over the active area. These are reference-derived diagnostic
values, unavailable to inference and not candidate-selection or oracle routing.
Scaling an already rounded residual is also not identical to rerunning a model
with a different blend.

This exposes an important shortlist limit: weaker patch-qualified strengths
were not evaluated on full images by the top-three protocol and may pass.
The comparison rejects its 18 measured finalists, not every weaker family
member. A complete fixed eligible-family evaluation or reference-free regional
control is the next concrete test; no final-image retuning is justified.
[Protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/shadow-protocol.json) and
[measured signed errors](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/shadow-analysis.json) retain
that distinction.

## Complete patch-qualified family follow-up

The measured shadow overshoot motivates removing the top-three shortlist
limit. A separate protocol freezes **all 48 patch-qualified settings** from the
unchanged training study before complete-image evaluation. It reuses the 72
previously checked source/candidate results, bound to their receipt hash, and
executes the remaining 120 inferences. No training, loss, checkpoint or patch
guard changes, and no final reference pixels are used. This is continued
development on reused validation data, not an unseen test.

**Ten of 48 settings pass all complete-image regional guards.** The selected
combination is preclean + detail + structure, step 2000, output blend 1/16.
Pooled active improvement is **+0.00735166 dB**; current BIBO on those same
validation sources is **+0.02782028 dB**, and the separately tested BIBO
intensity gate reaches +0.08716186 dB. These are validation comparisons, not
comparisons of historical final results or visible detail.

The initial shortlist did miss passing weaker settings; its failure cannot
reject the whole family. The complete evaluation closes that selection gap.
The full recipe combination performs best in this small experiment, yet offers
no quality improvement over the existing model. Its parameter/cost difference
may warrant a separate measured tradeoff; no speed advantage is assumed.

The [frozen diagnostic](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/frozen.json) binds the exact
configuration/checkpoint without publishing it as a replacement. Historical
final evaluation and native/Pi integration are not performed for this inferior
validation candidate. No passing criterion is relaxed and no original approval
is transferred. Larger models, calibration/domain conditioning and separate
component/seed comparisons remain open.

[Complete-family protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/all-full-protocol.json),
[results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/all-full-selection.json) and the
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/validate_stock_sr_recipe_all_qualified.py) retain
all 192 complete-image source/candidate results, including the 72 reused
results. All seven hosted jobs pass at published evidence revision `3ce777f`
([receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-recipe-receipts/passed-3ce777f.json)); new evidence heads
require their own hosted results. Runtime/model source is unchanged.
