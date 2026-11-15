# Region-balanced 2x loss: rejected; full-frame validation exposes a gap

This fresh study improves the previous 2x model's shadow/highlight behavior on
four historical references, but regresses overall and detail error on two.
It is **not promoted**, and no weights are added to the repository. The optional
1x checkpoint and production runtime are unchanged.

## Training and frozen selection

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/protocol.json) precedes training.
It uses the same audited RGGB14/Q3 dataset: 512 training patches, 128 validation
patches from four separate validation sources/two groups, and no final-test
patches. Training is fresh `F_ane`, width 16/depth 3, seed 20261031, MPS,
3000 steps, batch eight, learning rate 0.0003 and residual scale 0.01.

Instead of an exponential dark-count weight, the training objective is the
mean of four per-patch MSE ratios: overall, shadow, highlight and detail,
each divided by that training patch's rounded bicubic baseline MSE. Denominators
have a one-count-squared floor. Reference-only masks match `region_totals`.
This gives the regions equal relative weight rather than letting large
absolute errors dominate. Weight construction reads **training patches only**;
validation and final pixels are excluded. It can overweight low-error training
patches and is an experiment, not a general quality recommendation.

The [independent loss audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/loss-audit.json) compares
direct regional-ratio loss with the pixel-weight implementation on a nontrivial
perturbation: absolute difference 1.19e-8, below the 1e-6 check tolerance.
The [archived derived recipe](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/run-training.py) records
the exact experimental code; it is not the unmodified production training tool.
The protocol binds its hash and the underlying production tools.

Training takes 46.98 s. Selection checks the predefined ten 300-step snapshots
and 17 strengths, with the unchanged exact pooled/per-group regional guard.
Of 170 candidates, 113 qualify. Step 3000 at full strength wins: validation MSE
8766.4013 → 8474.9331, **+0.14685 dB**. More candidates increase validation
selection risk. The [freeze](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/frozen.json) binds
checkpoint `a350ecc8e5c95c692f21daff6049a19920525af615ca1c154fe81d903365f6b2`
before final evaluation; no final pixels select another snapshot or strength.

## Historical full-reference results

| Reference | Active gain, dB | Shadow gain, dB | Highlight gain, dB | Detail gain, dB |
|---|---:|---:|---:|---:|
| Z8Z_0001 | +0.082 | +0.275 | +0.413 | +0.266 |
| Z8Z_0067 | +0.209 | +0.725 | +0.229 | +0.221 |
| Z8Z_5323 | **-0.084** | +0.517 | +0.036 | **-0.009** |
| Z8Z_6693 | **-0.073** | +0.271 | +0.260 | **-0.007** |

Shadow/highlight error improves everywhere, but two active/detail regressions
fail the strict guard. This shifts the tradeoff rather than resolving it.
Independent lossless source decoding, identical historical stock-Q3 inputs,
editable output pixels and supported calibration are checked as in the prior
study. These are simulated low inputs and historical references, not newly
unseen sensor footage, temporal quality or broader-camera approval.

## Full frames from the validation split

After rejection, a separately recorded diagnostic runs the **same frozen
checkpoint** on all four complete validation sources. It does not retune or
reselect the candidate. Their sampled patches passed every pooled/per-group
validation guard, but three full images fail:

| Validation source | Active gain, dB | Shadow gain, dB | Highlight gain, dB | Detail gain, dB | Guard |
|---|---:|---:|---:|---:|---|
| ForestFire_001 | +0.079 | +0.018 | +0.175 | +0.118 | Pass |
| ForestFire_010 | **-0.007** | **-0.144** | +0.288 | +0.087 | Fail |
| ColoradoSky_001 | +0.199 | **-0.454** | +0.278 | +0.237 | Fail |
| ColoradoSky_010 | +0.286 | **-0.729** | +0.326 | +0.306 | Fail |

This is direct evidence that sampled patch validation does not establish
full-image nonregression, even on its own held-out source split. Patch coverage,
regional masks and full-frame tiled execution differ; this experiment does
not isolate their individual contributions. A subsequent selection study
should include full-frame validation guards **before** final-reference testing,
rather than selecting strengths from failed final images. No approval follows
from the diagnostic, and the original failure is retained.

## Reproduction and receipts

[Training](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/training.json),
[binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/training-binding.json),
[selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/selection.json),
[selector](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/run-selection.py),
[final driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/run-final.py),
[final summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/final-summary.json), and
[split audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/split-audit.json) bind the study.
Full-reference results:
[0001](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/final-0001.json),
[0067](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/final-0067.json),
[5323](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/final-5323.json),
[6693](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/final-6693.json).
The full-frame validation has its own
[protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/full-validation-protocol.json),
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/run-full-validation.py),
[summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/full-validation.json), and
[source 0](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/validation-0.json),
[source 1](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/validation-1.json),
[source 2](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/validation-2.json),
[source 3](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-relative-receipts/validation-3.json) receipts.

Photos, patch arrays, weights and full image outputs remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_region_relative_20261030`.
Archived drivers identify the measured installation; adjust paths and choose
fresh output roots to reproduce. No model is downloaded or selected by default.

The [full-image selection stage](stock-sr-full-selection.md) is now implemented
and tested on Mac/Pi. Ten patch-qualified snapshot/strength candidates all fail
its complete-image guard, so this follow-up selects no checkpoint and skips
final-reference testing. No failed weights are promoted.
