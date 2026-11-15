# Focused stock-Q3 1x cleanup experiment

The fresh RGGB14/Q3 experiment did **not** produce an approved model. All 30
evaluated checkpoints failed the prespecified validation region guard. The
selected checkpoint remains the zero-residual, step-zero identity fallback.
Ordinary decoding and default model selection are unchanged. Extending every
format and preset through model training remains stretch work.

A subsequent [residual-strength study](stock-cleanup-strength.md) preserves
these full-strength failures, then selects half strength using validation only.
That separately frozen 1x candidate passes the declared validation and four
full-reference numeric guards. This earlier experiment remains the unchanged
full-strength/identity-fallback result below.

## Data and selection

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/protocol.json) was written before building
the dataset or training. The same sources as the [2x experiment](stock-sr-training.md)
supply eight training frames in four groups, four validation frames in two
groups and four final references. Exact source bytes and reference RAW hashes
are disjoint across splits. Scene labels are caller supplied; this does not
exclude perceptual near-duplicates. Final references have appeared in historical
studies and are not a newly unseen benchmark collection.

Each lossless source is independently decoded with the SDK and
tifffile/imagecodecs, then its original-resolution Bayer frame is encoded and
decoded with stock Q3. The builder draws 512 training and 128 validation
128x128 Bayer-plane patches inside the active area on an eight-pixel grid.
Final-reference pixels never enter the arrays. The
[dataset receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/dataset.json) records crop coordinates,
source identities, decode checks and array hashes; the
[split audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/split-audit.json) checks final RAW hashes.

Fresh seeded `F_ane_no_sr` weights use width 16/depth 3, a zero residual head,
residual scale 0.01 and normalization 16383. AdamW runs 3000 steps, batch 8,
learning rate 0.0003, weight decay 0.0001, seed 20261029. Loss on the central
64x64 output core weights squared normalized errors by
`1 + 3 * exp(-target_counts / (0.05 * 16383))`, then scales the mean by 10000.

Every 100 steps, selection compares rounded deployment-domain counts. It
requires lower pooled validation MSE and no increase relative to identity in
overall, shadow, highlight or detail MSE, both pooled and in **each** validation
group. Masks use each reference patch's bottom/top decile and top-decile
same-site absolute gradient, independently of predictions. These patch masks
differ from the final evaluator's whole-active-area masks. Step zero remains
available if no candidate passes.

## Result and evidence

Training took 41.79 seconds on MPS. Lowest overall validation MSE occurred at
step 1300, a **0.656 dB** gain. It increased pooled shadow/detail error,
ForestFire shadow/detail error and ColoradoSky shadow error. Every other
evaluated checkpoint also failed at least one constraint. The
[training history](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/training.json) and
[rejection list](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/selection-rejections.json) preserve this
result. The historical `delta_vs_bicubic_db` field is a compatibility alias;
for this 1x run both delta fields compare against identity.

The fallback SHA-256 was frozen before final evaluation:
`a4dcde2496477ff50b6b8473d98ab286a11ecea484eca12d30047deb3a46bcd5`.
The [freeze receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/frozen.json) records the training
and protocol hashes at that boundary; `final_evaluation_started=false` records
its state when written. The subsequent [final summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/final-summary.json)
confirms zero changed samples on all four full-resolution Q3 editable DNGs,
with exact calibration and pixel checks in each final receipt. This validates
the fallback transport, not a learned quality improvement. The companion split
audit supplies provenance beyond the generic evaluator's split-unknown disclaimer.

All seven optional reconstruction contracts pass, including 1x runtime
checkpoint loading, exact identity fallback and rejection of a per-group
shadow regression. See [test output](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cleanup-receipts/contracts.txt).
Weights and photographs remain outside the repository under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_cleanup_20261029`.
No sanitizer campaign, remote transfer or model promotion was part of this work.
Perceptual, temporal and camera quality approval remain open.

## Reproduction

Use an external manifest with disjoint `train`, `validation` and `test` groups,
schema `gpr.stock_sr_sources.v1`:

```sh
python tools/train_stock_sr.py build --sources sources.json --sdk /path/to/gpr_tools \
  --output-dir /new/dataset --scale 1 --seed 20261029
python tools/train_stock_sr.py train --dataset /new/dataset --output-dir /new/train \
  --device mps --steps 3000 --batch 8 --learning-rate 0.0003 \
  --validate-every 100 --seed 20261029 --shadow-weight 3 --guard-regions
```

Existing callers retain the default scale 2. Training requires Torch, NumPy
and tifffile/imagecodecs; ordinary encoding and decoding do not.
