# Historical-width SR: boundary handling, RAW fidelity and rendered tradeoff

A frozen historical-width candidate now passes the declared RAW regional
guards on complete validation images, four historical references on CPU/MPS,
and the supplied GP011689 GoPro simulation. Its rendered RGB improvement on
that GoPro scene is much smaller than the supplied BIBO model's. It remains
an analysis-only RAW-fidelity diagnostic; no supplied checkpoint or production
backend is replaced.

## Fixed comparison

The checkpoint is the preceding [historical-width study](stock-sr-historical-recipe.md)'s
local-shadow step-400 continuation. Weights are unchanged. Four correction
strengths (1, 1/2, 1/4, 1/8) cross four input-only gates: none, and the three
fixed ramp arrays from the earlier BIBO gate study. No new intensity threshold
is fitted to reference pixels. Gate inputs are per-Bayer-site current bicubic
counts divided by 16383.

The correction operates on independently rounded model/bicubic counts in
float64, followed by half-up count rounding. This is an explicit new inference
contract, distinct from the earlier BIBO float-network gate. It does not use
reference images, masks or per-image fitted parameters at inference.

Sixteen patch settings are scored on the existing 128 validation patches.
Thirteen pass the unchanged pooled/per-group four-region nonregression guard.
Every passing setting then crosses two boundary policies and runs on all four
complete validation images: **26 candidates, 104 outputs**. There is no
shortlist. All baseline pixel hashes and metrics reproduce prior independent
receipts; the historical full-strength control reproduces its previous output
hashes and metrics exactly.

## Boundary implementation and checks

The historical tile-512/overlap-32 origins and central crop rules remain the
same. Only tiles touching the actual frame boundary gain 32 reflected
low-plane samples outside that edge. Coordinates extend linearly using the
original complete source dimensions; reflection never changes Bayer-site
identity. Interior tile borders retain the original overlap policy.

Three CPU cases include nondivisible plane dimensions 97x113, 128x160 and
160x192 with tile 96/overlap 32. The new unextended tiler reproduces the
archived historical tiler exactly. Reflected tiled output agrees with an
independent whole-image reflected forward pass within one count; two cases
are count-identical. Coverage checks reject unwritten output pixels.

## Complete-image selection

| Frame policy / correction | Pooled active gain versus bicubic | Regional result |
|---|---:|---|
| Historical / full strength | +0.57218 dB | Three images' shadows fail |
| Reflect / full strength | +0.57404 dB | Two sky images' shadows fail |
| Historical / strong gate, full strength | +0.53889 dB | All pass |
| Reflect / strong gate, full strength | **+0.53917 dB** | **All pass; frozen** |

Seventeen of the 26 settings pass. The frozen setting has the lowest eligible
pooled active MSE. Its gate ramps are `[0.03, 0.15, 0.65, 0.9]`, with strength
1. Reflection fixes the first forest image's shadow failure, but does not
resolve interior sky shadows on its own. The gate alone qualifies; reflection
adds only about 0.00027 dB to the gated result. The factor comparison does not
support calling reflection the main quality improvement.

The [selection audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-boundary-receipts/selection-audit.json) verifies
all final candidate/output hashes, regional decisions, pooled weighting and
complete qualified-family coverage before writing the immutable
[freeze](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-boundary-receipts/frozen.json).

## Frozen historical references and camera transfer

The frozen setting then runs on the four historical Nikon references on CPU
and MPS, without selection or retuning. All **32 regional comparisons** pass;
pooled active gain is **+0.120380 dB MPS / +0.120380 dB CPU**. Each CPU/MPS
output differs by at most one count. These are reused historical references,
and overlap with old pretraining is not excluded; this is not a blind test.

On the supplied GP011689 reference simulation, all four RAW regions pass at
**+1.13707 dB** versus bicubic, compared with the supplied BIBO model's
**+0.08283 dB** on the identical source/stock-Q3 low counts. Active RMSE falls
from 23.27134 to 20.41587 counts. The shadow region is count-identical to
bicubic. The independent editable-DNG read matches the new output exactly,
and supported high calibration, including the full-frame Bayer GainMaps,
roundtrips exactly. Opaque source XMP follows the existing omission policy.

This camera reference is already compressed GPR counts, not lossless sensor
truth. Same-site area simulation plus stock Q3 is not a native synchronized
12/50 MP capture pair. The reused household scene is not newly unseen.

## Rendered appearance prevents replacement

Reference, bicubic, supplied BIBO and frozen historical outputs are freshly
rendered with identical supported calibration and matched CIRAWFilter
settings, into 16-bit RGBA Rec.709 TIFFs. Three structure-oriented crops are
fixed from the old reference-only overview before viewing the new output.
Metrics exclude alpha and measure encoded RGB/Y-prime fidelity, not
scene-linear radiance or recovered sensor detail.

| Model | RAW active gain | Rendered RGB gain | Rendered Y-prime gain |
|---|---:|---:|---:|
| Supplied BIBO | +0.08283 dB | **+0.28430 dB** | **+0.37089 dB** |
| Frozen historical diagnostic | **+1.13707 dB** | +0.00373 dB | +0.02313 dB |

An independent row-chunked audit recomputes RGB error per channel and
Y-prime squared error from diagonal/cross terms, with floating exceptions
raised. All scores agree within the fixed 1e-10 relative tolerance. The
larger RAW improvement does not establish better rendered appearance.

The input-only gate is zero for **79.9911%** of this camera simulation's
samples, and those output counts equal bicubic exactly. Only **9.4286%** of
all counts change after rounding. The training sources have black level 1008;
this GoPro file has black level 16. The gate uses absolute normalized counts,
not black-level-normalized signal. Those facts explain its different coverage
across sensor domains; they do not isolate every cause of the rendered gap.

Visual inspection of the matched 1:1 paper-text, furniture-edge and rug crops
shows persistent softness relative to the reference, especially in dark
structure. The new output often stays close to bicubic there. Noise and
color artifacts remain in the dark furniture crop. Neither the RAW score nor
this one scene establishes useful restored texture or temporal quality.

Local review:
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_sr_historical_boundary_20261031/render-review/comparison.png`.
Full 16-bit masters and editable outputs stay beside the external receipts.
Photos, RAW arrays and model weights are not uploaded with this study.

## Remaining work and reproducibility

This establishes a stronger compatible-input RAW diagnostic, but gives no
reason to replace the supplied model for rendering. Next comparisons need
sensor-calibrated signal handling and appearance-sensitive training on
properly audited training sources. The frozen final/camera results are not
used to retune this candidate. Standard/native loaders still do not implement
this six-channel coordinate architecture, reflection policy or count gate.
No new Pi performance, native backend, temporal or restored-tier approval is
claimed.

The [manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-boundary-receipts/manifest.json) binds protocols,
source snapshots, tiler checks, all selection/final results, camera/renderer
receipts, independent render and delivery audits, and the prior c7125ab
seven-job CI receipt. Runtime source and supplied checkpoints are unchanged.
