# 2x strength and shadow-weighted follow-up: not promoted

Neither 2x follow-up below supplies an approved checkpoint. The existing
full-strength 2x candidate remains unapproved, and no new 2x weights are added
to the repository. The separately validated [1x candidate](stock-cleanup-strength.md)
is unchanged. Guards are not relaxed to convert an aggregate gain into a pass.

## Existing candidate, reduced strength

A [preregistered family](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/rejected-unweighted-protocol.json)
tests strengths `1, 1/2, …, 1/65536` for the frozen fresh-trained checkpoint
`3a6fa544…`. It reproduces the original identity-bicubic and full-strength
validation MSE exactly. All 17 strengths regress in pooled shadows and in both
validation groups' shadows. Even the smallest strength changes 858 validation
samples and fails. No strength qualifies; no further final-reference evaluation
is performed for that family. [Complete selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/rejected-unweighted-selection.json).

## Fresh shadow-weighted model

The next [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/protocol.json) is recorded before
training: the same source-grouped RGGB14/Q3 scale-two dataset, fresh seed
20261030, width 16/depth 3 `F_ane`, training residual scale 0.01, 3000 steps,
batch eight, AdamW learning rate 0.0003 and validation every 100 steps. The
existing exponential shadow-loss weight becomes 20. This changes the training
objective, not the pooled/per-group overall/shadow/highlight/detail guards.

Training takes 44.19 s. The provisional lowest-overall-validation-MSE snapshot
is step 2400, MSE 8766.4013 → 7877.7959. The same predefined strength family
then applies the unchanged exact regional guard on validation only. Eleven
strengths qualify; **1/16 strength** wins among them, residual scale 0.000625,
MSE **8629.5223**, only **+0.068 dB** over clipped per-plane bicubic.

The [freeze receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/frozen.json) binds selected
checkpoint `2c9abcf944fbb01eb7e12552957db298771ae4ed04d24e294a4c9163f0577d06`
before the next four-reference evaluation. Final pixels do not select another
strength, and no retuning follows the failed results below.

| Reference | Active gain, dB | Shadow gain, dB | Highlight gain, dB | Detail gain, dB |
|---|---:|---:|---:|---:|
| Z8Z_0001 | +0.069 | +0.048 | +0.060 | +0.074 |
| Z8Z_0067 | +0.038 | **-0.026** | +0.047 | +0.054 |
| Z8Z_5323 | +0.009 | +0.014 | +0.003 | +0.008 |
| Z8Z_6693 | +0.014 | +0.041 | **-0.031** | +0.009 |

Overall PSNR improves on all four, but shadow error on 0067 and highlight error
on 6693 violate the declared nonregression guard. The candidate is rejected
for promotion. It remains an external research artifact.

Both independent source decoders agree. Low stock-Q3 decoded inputs and full
reference RAW hashes match the prior study exactly; editable model counts and
transformed supported calibration are independently checked. The
[split audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/split-audit.json) confirms exact source
identity/RAW separation and no test patches. Historical references have already
appeared in earlier studies; they are not newly unseen data. Perceptual
near-duplicates, wider cameras, sensor footage and temporal quality are not
established. Simulated low inputs use same-site rounded 2x2 averaging before
actual stock-Q3 encode/decode, not a sensor or the old fused decimator.

[Training](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/training.json),
[binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/training-binding.json),
[validation selection](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/selection.json),
[final summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/final-summary.json) and the individual
[0001](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/final-0001.json),
[0067](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/final-0067.json),
[5323](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/final-5323.json) and
[6693](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/final-6693.json) results retain the failures.
The [original-model selector](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/run-unweighted-selection.py),
[fresh-model selector](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/run-selection.py) and
[final driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-balanced-receipts/run-final.py) identify the measured
installation and external data/model paths. Training uses the existing recipe
with the protocol parameters; final evaluation is skipped if validation has
no eligible nonzero candidate.

The subsequent [region-balanced loss study](stock-sr-relative.md) improves
shadows/highlights on all historical final references but regresses overall
and detail error on two, so it also remains unapproved. Full-frame diagnostics
on its four validation sources fail three guards despite passing patch guards;
future selection needs full-frame validation before final testing.
