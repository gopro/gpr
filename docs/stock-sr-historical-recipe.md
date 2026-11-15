# Stock SR: historical architecture, starting weights and shadow supervision

The full historical width-96/depth-6 architecture produces substantially larger
stock-pair fidelity gains after fine-tuning than the earlier small recipe
adapters. It has not yet qualified as a replacement: the first shadow-constrained
family passes patch checks but fails full-image shadows. Production weights and
runtime remain unchanged. The subsequent
[boundary/input-gate comparison](stock-sr-historical-boundary.md) qualifies a
frozen RAW-fidelity diagnostic, but its rendered GoPro gain remains below the
supplied model's; no replacement is promoted.

## What is reconstructed

The old selected checkpoint is the 633,444-parameter
`coord_detail_preclean_adapter_pixelshuffle` model, with residual scale 0.3,
two global coordinate channels, four same-site detail channels, a preclean
adapter and a PixelShuffle head. Every tensor of the selected historical
checkpoint is verified to equal the half-weight interpolation of its recorded
step-1/step-200 endpoints. This is parameter interpolation, not output blending.

There is a historical metadata discrepancy: the frozen registry says gradient
weight **0.4**, while the actual selected-endpoint training receipt says
**4.0**. These experiments use the receipt's 4.0, with Laplacian 0.05,
detail-phase 0.1, preclean auxiliary 0.02 and preclean-detail auxiliary 0.03.
They use the located historical trainer's Charbonnier/structure functions.
That receipt does not bind the trainer source hash or learning rate; this is
a documented reconstruction, not a bit-identical replay of old training.

Training uses the existing audited stock-Q3 Nikon pairs: eight training sources,
512 training patches, four validation sources and 128 validation patches.
The validation sources belong to two scene groups. Test references have zero
patches in these arrays. Crop coordinates come from the dataset's recorded
origins and the original complete source dimensions; scalar endpoint checks
verify that coordinates are global. The supervised core excludes patch borders.
The preclean target is the new stock task's rounded same-site 2x2 average;
the old Gaussian-area input contract is not inherited.

Current-update validation is held out, but overlap with old pretraining has not
been excluded. These reused small Nikon sources do not establish native GoPro,
blind-test, visible-detail or temporal performance.

## Matched starting-weight comparison

Two cases share architecture, losses, seed, source-balanced minibatch sequence,
400 updates, batch eight and AdamW learning rate 0.0001. One starts from the
old selected weights; the other starts fresh. Each evaluates step-200/step-400
weights and their half-weight interpolation with step 1. There is no
output-strength sweep.

| Starting weights | Step-400 patch PSNR gain versus current bicubic | Patch guard |
|---|---:|---|
| Old pretrained | +0.97659 dB | Fails pooled and both groups' shadows |
| Fresh | -0.76376 dB | Fails all 12 regional comparisons |

None of the eight candidates qualifies. Pretrained step 400 reduces pooled
overall MSE by about 20%, but increases pooled shadow MSE by about 2.2%.
Under this fixed training budget, the old starting weights are useful;
architecture size alone does not explain the result. This does not isolate
each historical architecture component or prove a universal pretraining gain.

## Matched local-shadow continuation

Both cases start at the preceding pretrained step-400 checkpoint, reset AdamW
to 0.00003 and share another 400-update sequence. The control retains the same
losses. The other adds a training-only hinge penalty with weight 0.02 on each
training patch's shadow MSE divided by its bicubic shadow MSE, with margin 0.98
and denominator floor one count squared. Reference-derived masks never enter
inference; nontraining mask arrays are checked to remain zero.

All four shadow-constrained candidates pass patch regional guards at
+0.95720 to +0.96773 dB. All four control candidates fail. Every patch-qualified
candidate then runs on every complete validation image, with the unchanged
per-image active/shadow/highlight/detail nonregression criterion.

| Complete-image candidate | Pooled active PSNR gain | Failed shadow images |
|---|---:|---:|
| Step 200 | +0.55848 dB | 3 of 4 |
| Half-weight interpolation, step 1 to 200 | +0.56168 dB | 3 of 4 |
| Step 400 | +0.57218 dB | 3 of 4 |
| Half-weight interpolation, step 1 to 400 | +0.56958 dB | 3 of 4 |

The full-image step-400 shadow MSE ratios versus bicubic are 1.03159,
0.96464, 1.09387 and 1.18567 in the fixed validation-source order. Every other
required region passes. No candidate is selected or sent to final-reference
evaluation. Patch success does not override complete-image failures.

Complete-image inference uses the exact archived historical tiler, tile 512,
overlap 32 and global coordinates. Its original frame-edge policy differs
from the current stock bicubic halo policy; both are explicit. Bicubic pixel
hashes and all metrics reproduce the prior independent complete-image receipt
exactly. Full validation records PyTorch 2.12.0; the earlier training reports
record 2.8.0. No cross-version model-output identity is claimed.

## Full-source shadow supervision follow-up

The next controlled comparison adds the complete training source's shadow
mask and bicubic error normalization to the local-shadow objective. It uses
only the eight training sources, verifies complete reference hashes and every
crop's target pixels, and checks the audited whole-source shadow thresholds.
The local-only control and combined case start from the same pretrained
step-400 checkpoint and use the same sequence, optimizer and losses.

Both cases complete 400 updates. Four local-only candidates and two combined
candidates pass the patch guard; all six run on all four complete validation
images, with no shortlist truncation. No candidate passes the full-image guard.

| Combined-mask candidate | Full-validation pooled active gain | Full-image failures |
|---|---:|---|
| Step 400 | +0.40195 dB | Three shadow regions and first forest image's active region |
| Half-weight interpolation, step 1 to 400 | +0.52516 dB | Three shadow regions |

The matched local-only control reproduces the earlier outcome to about
0.000002 dB at patch level; exact model-output identity across the two recorded
PyTorch versions is not claimed. This experiment uses PyTorch 2.12.0. It
does not establish the crop-versus-source mask difference as the sole cause
of the earlier failures. The combined loss improves some shadows but trades
away other fidelity, and is not selected.

## Shadow-error attribution

A separate validation-only diagnostic measures baseline error E and model
correction R in the existing complete-image outputs. It reports R squared,
2E.R and their sum, splitting shadows into the outer 64 Bayer-pixel frame
border and the remaining interior. No thresholds are fitted for inference.

For the local-only step-400 model, frame-border error accounts for about
53% and 61% of the two sky images' total shadow-error increase. Their interior
shadow MSE still increases by 4.71% and 7.73%. The first forest image improves
in its interior but loses that improvement at the border. The combined-mask
model also has interior sky regressions, about 6.91% and 4.05%.

The archived frame-edge policy therefore deserves an explicit padding
comparison, but an edge fix alone is not supported as a complete solution.
The interior decomposition also shows negative 2E.R and larger positive
R-squared terms for the failing local-only sky shadows: correction overshoot
is present, rather than an entirely wrong average direction. These are
diagnostic reference-dependent measurements, never inference features.

## Reproducibility and scope

The [receipt manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-historical-receipts/manifest.json) binds frozen
protocols, training reports, evaluation code, selected historical metadata and
complete-image results. Photos, arrays, output RAWs and model weights remain
external. The original import-setup failure and a later missing-imagecodecs
setup failure are retained; neither is counted as successful training.

The three comparisons complete 2,400 optimizer updates and 40 full-image
candidate inferences. The [audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-historical-receipts/audit.json)
recomputes every candidate's patch/full regional qualification, verifies actual
checkpoint/output hashes and exact parameter-interpolation identities, and
checks complete patch-qualified family coverage. A trainer serialization
limitation is explicit: step-200/400 event hashes describe the initial save,
subsequently overwritten at the same path with candidate metadata. Final
candidate hashes bind the files actually evaluated; those final state tensors
are used for interpolation checks. No earlier serialization byte identity is
claimed.

The missing-imagecodecs run stops during mask construction before training.
Its protocol/error are retained. The completed comparison uses an existing
environment with imagecodecs, without dependency installation or changed data.
These are
quality experiments, not production backend or Pi speed validation. No
production checkpoint, supported input contract or quality threshold changes.
