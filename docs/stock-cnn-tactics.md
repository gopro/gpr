# CNN tactics: historical evidence and research comparison

The stock rebuild implements the CNN media paths. A new
[historical-architecture study](stock-sr-historical-recipe.md) reconstructs the
old selected model and documented losses on stock pairs; it does not replay
the full historical training corpus or establish replacement quality. The
current stock candidate is a width-16 BIBO U-Net with a bicubic residual; the old
selected model is a width-96/depth-6 coordinate/detail/preclean adapter with a
PixelShuffle head. Runtime parity alone does not establish quality parity.

## What the older work actually selected

The frozen PR #95 [registry](https://github.com/dcliftreaves/gpr/blob/8be29eee57b4637865b572f18ab9e3f4edaa2236/pipelines/registry.json)
records `mission1_native12_8k_sr_q4t2_coord_detail_alpha0p5_v1`, checkpoint SHA
`791628778f74b13542677edeb5de7934b88d274531a8ec1755d8cf3d6388fb07`.
Its source model and registry are copied into the linked receipts. The associated
interpolation summary and broad decision bytes match their recorded registry
SHA-256 values exactly. Those are historical measurements for the old codec,
not fresh stock results or a clean per-component ablation.

| Tactic | Historical evidence | Stock rebuild implication |
|---|---|---|
| Match the actual codec input | Selected model trained on Mission 1 q4/t2 lows with a detail-residual sidecar and regenerated Nikon lows | Regenerate pairs through stock Q3; old weights/sidecar performance cannot be inherited |
| Pre-clean before enlargement | Separate low-resolution correction feeds the SR adapter | Explicit preclean supervision is a justified next architecture comparison |
| Give SR coordinates and detail | Two absolute coordinate channels and four deterministic same-site high-pass channels feed the adapter | Test these independently; coordinates must be global across tiles, and can also overfit position |
| Preserve structure in the loss | Registry specifies gradient 0.4, but the selected-endpoint training receipt specifies 4.0; both give Laplacian 0.05 and detail-phase 0.1 | The historical-architecture reconstruction uses receipt 4.0 and records the discrepancy; regional MSE alone does not reproduce these objectives |
| Balance hard examples against broad scenes | Historical current-contract summary says hard-only improvements regress Nikon; balanced/mixed training partly recovers it | Source-balanced mining with full-image validation, not repeated training on the worst crop alone |
| Interpolate checkpoint weights | Selected alpha 0.5 is interpolation between step-1 and balanced step-200 weights | This differs from multiplying one model's output residual by alpha; test correlated checkpoint interpolation separately |
| Check full images and rendered appearance | Broad receipts cover 42 Mission rows and 24 regenerated Nikon rows; 4K cleanup has separate RGB/CFA/tone review | RAW regional checks, calibrated render comparisons and temporal review need separate evidence |

The hash-verified broad alpha-0.5 decision contains positive per-image PSNR
changes relative to the prior preclean baseline, including +0.0411 dB on
GP017349. It is evidence for that complete selected recipe; it does not isolate
coordinates, losses or interpolation as the sole cause. The old 4K cleanup
registry uses a different RGB-target architecture (width 40/depth 5), gradient
weight 1, RAW weight 2 and Bayer weight 2, with visual review-only scope.

Historical failures also matter. The external current-contract summary at
`current_goal_sr_current_contract_summary_20260618/summary.json` rejects
hard-only mining, identifies generalization/detail-placement problems, rejects
residual-scale increases as a solution, and records a width-64/depth-10 scratch
probe stalled at its bilinear baseline through 2,000 steps. The later optional
PSF scoreboard has no promotable candidate. Neither larger networks, PSF work
nor teacher artifacts should be called a demonstrated unlock without their
actual matched results. Existing Restormer teacher artifacts show exploration;
this audit has not established a generalizable teacher-distillation win.

## Primary research and how it changes the next experiments

- [RealBasicVSR, CVPR 2022](https://arxiv.org/abs/2111.12704) finds cleaning useful
  before temporal propagation, which can otherwise amplify degradation. This
  supports testing explicit codec cleanup before SR. It does not prove the same
  magnitude of gain for our single-frame editable-Bayer path.
- [BSRAW, WACV 2024](https://arxiv.org/abs/2312.15487) uses RAW-domain sensor noise,
  defocus and exposure degradations. [Mosleh et al., CVPR 2026](https://arxiv.org/abs/2603.12493)
  reports benefits from calibrated device-specific degradation models over
  arbitrary degradation pools. Our inference: actual stock encoding in the
  pairs is appropriate, but simple area-downsampling on a small source set is
  insufficient evidence for real camera super-resolution. Preserve the exact
  synthetic task while adding a separately labelled realistic-degradation task.
  The 2026 model is RAW-to-RGB, so its output contract does not replace editable RAW.
- [BayerUnify/BayerAug, CVPRW 2019](https://openaccess.thecvf.com/content_CVPRW_2019/html/NTIRE/Liu_Learning_Raw_Image_Denoising_With_Bayer_Pattern_Unification_and_Bayer_CVPRW_2019_paper.html)
  explains why RAW augmentation must preserve CFA relationships. Expand scene
  coverage and use validated Bayer transforms; ordinary RGB flips are not an
  adequate specification for packed Bayer geometry.
- [NAFNet, ECCV 2022](https://arxiv.org/abs/2204.04676) supports efficient simple
  restoration baselines. Our ANE adaptation changes normalization and gating,
  so NAFNet's reported results do not qualify this implementation. Compare
  architectures on identical source splits, degradation, steps and losses.
- [Deep Network Interpolation, CVPR 2019](https://openaccess.thecvf.com/content_CVPR_2019/html/Wang_Deep_Network_Interpolation_for_Continuous_Imagery_Effect_Transition_CVPR_2019_paper.html)
  studies interpolation in parameter space between correlated networks. This
  directly distinguishes the old selected checkpoint interpolation from our
  current output-strength sweeps; interpolation needs common architecture and
  compatible training ancestry, not unrelated random checkpoints.

The next main architecture experiment should therefore compare stock-trained
preclean/detail and gradient/phase supervision against the current baseline,
with source-balanced training and identical complete-image checks. Coordinates
and checkpoint interpolation should have separate comparisons. More runtime
backend work is not the main unresolved quality problem. Temporal consistency
requires actual sequence evaluation; still-image PSNR cannot establish it.

## New validation-only spatial residual gate

This bounded diagnostic tests whether restricting residuals in dark/bright
normalized-count ranges permits stronger correction. Three fixed trapezoidal
intensity gates and six fixed strengths give 18 gated candidates; weak/full
controls give 20 patch configurations. Gates use the bicubic input only, per
Bayer site. No reference image or per-image fitted threshold is available to
inference.

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/protocol.json) precedes evaluation.
All 128 validation patches use the existing exact pooled/per-group regional
nonregression guards. The ungated 1/32 control exactly reproduces its previous
regional scores; cached masks are independently checked against the existing
evaluator. Eight gated candidates qualify. Patch-best strong-1/4 gains
+0.24327 dB versus +0.04383 dB control.

All eight gated candidates and the control then run on all four complete
validation sources, using hash-verified cached SDK reference/decoded-low RAWs.
Baseline pixels and metrics reproduce the previous benchmark; control output
pixels also reproduce their historical validation outputs exactly. The
[full result](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/full-selection.json) rejects patch-best
strong-1/4 for one image's shadows. **Strong-1/8** passes all sixteen regional
comparisons and is selected at **+0.08716 dB pooled active PSNR**, versus
**+0.02782 dB control**. The criterion is unchanged; the strongest patch result
is not relabelled as the selected full-image result.

The initial full driver stopped before candidate evaluation because a Python
active-area tuple was compared with its JSON list. Its protocol/code/error are
retained. JSON normalization fixes the representation comparison; numeric
values and bounds remain exact. The completed run has its own script hash.

The [freeze](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/frozen.json) binds base weights, gate,
strength and full selection before historical final evaluation. **Final
reference and CPU portability results are now available below. Editable/native
integration, useful visible detail and temporal quality remain unestablished.** It runs only
through the archived analysis wrapper; standard model loaders do not implement
this gate. Supplied model files and production runtime are unchanged. This is
an incremental diagnostic, not a substitute for reproducing the older recipe.

[Receipt manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/manifest.json) binds scripts,
protocols, results, frozen candidate and selected historical evidence. Large
arrays/photos stay external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_spatial_gate_20261030`.

## Frozen gate: historical-reference and CPU follow-up

The fixed strong-1/8 candidate now passes all 32 regional checks: four
historical references, four regions, two independent CPU/MPS baselines.
[Final protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/final-protocol.json) and
[results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/final-summary.json) preserve source, cache,
output and code hashes. Pooled active gain is +0.0228792 dB MPS / +0.0228791 dB
CPU, versus the supplied ungated diagnostic's +0.0072916 dB. All four CPU/MPS
outputs differ by at most one count under the existing portability budget;
[comparison](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-tactics-receipts/portability.json). No final pixels select
the gate or strength; these remain reused historical references, not blind data.

This is a modest numeric gain, not useful restored-detail approval. No gate
checkpoint replaces the supplied model, and standard/native/Pi media support
for this gate remains unimplemented. More important architecture comparisons
are proceeding against the audited older recipe.

The [eight-case recipe comparison](stock-sr-recipe-comparison.md) now tests precleaning, explicit detail and structure-loss combinations on the stock pairs. It completes 24,000 training updates and 72 full-image inferences; all 18 shortlisted candidates fail full-image regional guards, so no model is selected. Patch-level detail gains do not transfer into a qualified complete-image result.

A separate complete-family follow-up removes the recipe shortlist limit: all 48 patch-qualified settings are evaluated on complete validation images (192 total source/candidate results, 72 reused). Ten pass; the best full-recipe setting gains +0.00735 dB, below the current BIBO +0.02782 dB. It is retained as a frozen diagnostic, not a quality replacement. This corrects the shortlist gap without relabelling its failures.

The [boundary/input-gate follow-up](stock-sr-historical-boundary.md) crosses every patch-qualified correction setting with historical/reflected frame edges (104 full outputs), freezes the best passing RAW diagnostic, and verifies 32 historical-reference CPU/MPS regions plus four supplied-camera regions. Its +1.13707 dB GoPro RAW gain yields only +0.00373 dB rendered RGB gain, below the supplied BIBO +0.28430 dB. Fixed gate coverage and black-level differences are audited; no appearance or production replacement is promoted.

The [calibrated mixed-camera study](stock-sr-calibrated-mixed.md) recovers archived per-frame metadata, builds actual stock camera pairs, and compares sensor canonicalization/tone-WB loss in four matched cases. All 3,200 updates complete, but eight checkpoints fail Nikon shadow guards despite improved pooled RAW/proxy scores. The associated real-camera parameter-import overflow is fixed and checked on Mac/Pi; no new CNN is promoted.

The [sensor-gate and frozen-cascade follow-ups](stock-cnn-followups.md) complete the bounded comparisons without a replacement: 18 patch settings, 20 full gate outputs, ten complete cascade outputs and actual rendered comparisons. The only gate survivor is worse than supplied BIBO in pooled RGB; the cascade improves rendered Nikon fidelity but regresses every camera validation image. No thresholds or failed guards are relaxed. A separate [JSON boundary correction](stock-sdk-parameter-json.md) fixes the specific new hosted parameter-import failure, with fresh focused Mac/Pi passes.

The [matched BIBO joint-training study](stock-cnn-joint.md) now compares SR-only,
frozen-cleanup and jointly trained cleanup/SR on the same balanced stock pairs.
It preserves batch-normalization statistics and extends real input context
for the two-stage receptive field. Joint training improves pooled patch RAW
fidelity by +0.14394 dB versus supplied SR, exceeding both comparison arms,
but all six checkpoints fail Nikon shadow guards. Independent re-inference
and scoring confirm those results. No candidate advances to whole-image,
rendered or final evaluation, and supplied weights remain unchanged.

The [related-checkpoint interpolation study](stock-cnn-interpolation.md) now
tests actual parameter interpolation between supplied stock SR weights and
two fine-tuned descendants. All ten settings improve pooled fidelity but fail
Nikon shadows. Independent parameter checks and twelve repeated inference
settings confirm the result; no replacement is selected.

A new [Bayer-preserving translation diagnostic](stock-cnn-translation.md)
finds spatial sampling-phase sensitivity in the supplied 2x model. Integer
alignment leaves bicubic and sixteen-sensor-pixel CNN shifts exact, while
two-pixel CNN shifts differ by RMS 1.86..2.62 counts with rare maxima83..207.
A CPU control confirms this is larger than backend roundoff. This motivates
phase-consistency/anti-aliasing comparisons, not a real-video quality claim.

The [matched phase-consistency training study](stock-cnn-phase-training.md)
reduces pooled synthetic shift MSE by up to 84.7%/88.1%, but all six checkpoints
fail RAW fidelity guards and the regularized cases regress the Nikon tone
proxy. Independent replicated-edge checks reproduce phase results exactly;
no model, inference default or restored-tier approval changes.
