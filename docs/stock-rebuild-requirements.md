# Stock rebuild requirement ledger

Audit date: 2026-10-31. The latest SDK CLI parameter-import correction is
`170802d`, with all seven hosted jobs passing in
[run 41175893067](https://github.com/dcliftreaves/gpr/actions/runs/41175893067).
The earlier `779bb47` source/evidence verification and shared Python GainMap
simulation helper retain their focused Mac/Pi and real-camera receipts.
The streaming-row integration below adds optional encoder/capture APIs and a bounded preview consumer; older timing evidence remains revision-bound.
Encoder/reconstruction core remains `f1e08c8`; bounded preview-plane spacing
is implemented in `f197f09`.
Sources: the active goal, frozen runtime PR #95 (`8be29ee`), validation PR #96
(`821420d`), their PR bodies and adoption/validation files cited in the
[eight-group audit](stock-feature-parity-audit.md). Older receipts remain bound
to their recorded revisions; this ledger does not retimestamp their evidence.

Generated receipts are consolidated in the [evidence archive](stock-evidence/README.md),
with exact Git-object comparisons, per-file hashes, readable immutable links
and an offline verifier/restoration command. This corrects the reviewability
problem of roughly 688,000 added lines dominated by loose generated receipts.
The archive change passes fresh capture/preview/camera package checks and all
seven hosted jobs at `144a338`
([run 41176948842](https://github.com/dcliftreaves/gpr/actions/runs/41176948842)).
Later evidence changes still require their own hosted results.

All seven hosted jobs also pass at the subsequent joint-training evidence
revision `ffe8385`
([run 41177716051](https://github.com/dcliftreaves/gpr/actions/runs/41177716051)).
New diagnostic-only evidence below does not change runtime source or models.

## Requirements and evidence

| Requirement | Evidence inspected | Disposition |
|---|---|---|
| Separate branch and reviewable fork PR; preserve old PRs | `feature/stock-raw-media-runtime`; fork [draft #101](https://github.com/dcliftreaves/gpr/pull/101). GitHub reports fork #95/#96 and upstream #71/#72 open, with unchanged frozen head identities | Delivered. No upstream replacement promotion or old-PR closure |
| Build on stock encoder/#99/#100 | Foundation `d3d96ec`; historical payload/pixel comparisons in [core still receipts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/stills-mac.json), replay and [encoder evidence](stock-encoder-sub100.md) | Implemented and measured. Compatibility applies to checked codec profiles, not every container/editor |
| Broad stills and explicit compression choice | Four CFA layouts, 12/14/16-bit counts, packed-12 SDK support; `bayer_formats`, `raw_media`, `stock_still`; [presets](stock-still-quality.md) | Implemented. Historical stock log transform remains lossy |
| Original large-image/X2D case | [X2D receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-still-quality-receipts/mac-x2d-3265.json): 11664x8750 RGGB16, independent source decode, six presets, three repeats | Verified on one real 102 MP source. Broader corpus not established |
| Supported calibration and editable RAW | Typed image/EXIF/GPS/opcode records, decoded DNG/GPR count identities and source hashes; [contract](stock-editable-raw.md), `raw_media`, `stock_still`, real export/native integration receipts | Implemented within documented tag/opcode boundaries. General editor/lens compatibility is not established |
| Bounded recording lifecycle | Public C/C++ API; `capture_lifecycle`, `capture_c_api`; [contract](stock-capture-api.md) | Copies/leases, backpressure, asynchronous writes, cancellation, flush/finish and latched errors implemented. Sensor/DMA/storage adapters unverified |
| Checked RAW-video framing/recovery | `gvid3_conformance`, `gvid2_compatibility`, `video_format_matrix`; [contract](stock-capture-api.md) | Actual PTS, source/frame calibration, CRC/END3 and explicit prefix recovery implemented. Old fused GVID is a different profile |
| Full-frame calibrated 1024x768 preview without camera CNN | Independent fixtures and 192-case plan/plane comparisons; preview/capture tests; [Pi evidence](stock-capture-preview.md) | Implemented. Quarter-scale encoder-derived route optional. Memory sink tested; rear-display integration unverified |
| Relevant end-to-end Pi correctness/performance | Exact real-source replay; 600 recorded/previewed frames; [sustained receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoder-preview-600.json), [arena follow-up](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoder-arena-600.json) | Correctness verified. Latest [fixed FIFO profile](stock-process-scheduling.md) passes two 600-frame validations at 20.0056/20.0065 durable fps with no late writes/previews. The later [native 12 MP GoPro repeats](stock-native12-gopro-timing.md) both fail combined timing (600/570 accepted; 223/570 late previews), while all accepted bytes/calibration/previews remain exact. Broader reproducibility remains open; earlier receipts retain their results |
| Exact MOV, large files, PCM/timecode | `stock_mov`; [contract](stock-mov.md), 4.42 GB boundary receipt, [real A/V receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/real-core.json) | Implemented for declared private stock profiles. Exact recovery does not imply standard CFHD or universal editor support |
| Calibrated deep ProRes/editable outputs | `stock_prores`; [contract](stock-prores.md), real 120-frame outputs and [native integration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-native-receipts/real-integration.json) | Implemented on macOS with optional CNN and audio/timecode. Broader rendered look/motion approval open |
| Optional FFmpeg decoder equivalent | Pinned n8.0 adapter; twelve CFA/depth checks, fixtures, malformed configuration and conversion on Mac/Pi/hosted Linux; [contract](stock-ffmpeg-decoder.md) | Implemented as an explicit patched build. Bayer output is not calibrated color rendering |
| Portable offline 1x cleanup and 2x/8K inference | Eight `stock_cnn_test.py` contracts; [runtime](stock-cnn-portable.md), editable media and separate lossy re-encoding identities | Implemented for the declared RGGB14 family, explicit caller weights and optional repository 1x/2x research candidates. Broader codec formats do not widen model support |
| Original native desktop backend capability | Core ML, MPSGraph, custom Metal hybrid; `stock_coreml_test.py`, `stock_metal_test.py`, `stock_native_test.py`; [runtime](stock-native-runtime.md) | Implemented, including standalone host processing without Torch/NumPy runtime. Explicit numeric tolerances; historical monolithic CLI syntax not retained |
| High-level UPRESABLE modes/media chain | `stock_upres_test.py`, `stock_sequence_test.py`; [workflow](stock-upres-workflow.md), real Mac native and [Pi receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-upres-receipts/pi5-real-captured.json) | Timelapse/regression/both implemented through analysis workflows. Captured-GPR timelapse additionally has a standard-library native path. Native simulation/regression uses the analysis workflow |
| Real Pi reconstruction correctness/cost | Two 4096x3072 captures become 8192x6144 GPR/GVID/MOV; final payloads checked against independent historical encoder, editable pixels/calibration verified | Earlier candidate verified offline: 114.24 s total, 57.12 s/frame. The later [supplied diagnostic](stock-sr-source-regions.md) separately verifies 50 MP editable output/count agreement at 86.29–99.89 s/frame inference. Neither result is real-time CNN or sensor ground truth |
| Compatible checkpoints and stock-input quality validation | Architecture/domain/hash checks; source-grouped training and frozen evaluations in [1x](stock-cleanup-training.md) and [2x](stock-sr-training.md); legacy transfer also evaluated | A separately frozen [half-strength 1x candidate](stock-cleanup-strength.md) passes validation and the declared four-reference numeric guard (+0.639 dB pooled active PSNR), with Pi execution checked. The earlier full-strength 1x and stronger earlier 2x failures remain recorded. The later [source-wide 2x diagnostic](stock-sr-source-regions.md) passes patch/full-image/four-reference CPU/MPS numeric guards, but pooled gain is only +0.00729 dB. Broader visual/temporal/restored-tier promotion remains open |
| Original compact restored-tier claims | Six measured stock presets and explicit model/final decoded metrics | Old size/quality approvals do not transfer. No replacement restored tier or temporal/perceptual model approval |
| Independent packages/public clients | `build_profiles.py`, stripped capture/preview/camera/full packages, relocated C/C++ consumers, installed native receipt | Implemented. Apple/native ML optional and outside camera-library dependencies |
| Applicable validation/reproducible evidence | Old-to-new test map in [audit](stock-feature-parity-audit.md); 27 Mac/24 Pi local Release targets; optional CNN/native/FFmpeg tests; seven hosted jobs and hashed receipts | Implemented; hosted status evaluated per revision. Broader fuzz/sanitizer expansion deferred by user. Old commit-adoption/coverage instrumentation and research diagnostics not recreated |

## Remaining work

1. Keep hosted validation current as the branch advances. All seven checks for
   revision `40aa476` passed in
   [run 41139092733](https://github.com/dcliftreaves/gpr/actions/runs/41139092733),
   including the corrected explicit wavelet-queue test target, native backends,
   FFmpeg and packages. The [preview-spacing source](stock-preview-colors.md)
   separately passes 27 Mac/24 Pi Release targets and four relocated profiles.
2. The [selected 1x candidate](stock-cleanup-strength.md) now passes the declared
   patch-validation and four-reference numeric nonregression guards, with an
   explicit repository checkpoint and Pi execution evidence. The later
   [2x diagnostic](stock-sr-source-regions.md) also passes narrow numeric guards
   and Pi execution/count checks, with small benefit. Improve useful 2x detail
   quality and broader real-source, rendered visual and temporal evidence before
   promoting restored tiers. [Native artifacts for this checkpoint](stock-sr-native.md)
   now pass scoped execution/count and full-reference RAW-quality checks. Earlier failures remain recorded; the scoped 1x result does
   not approve other domains or video quality.
3. Extend sustained Pi deadline robustness beyond the [scoped FIFO profile](stock-process-scheduling.md). Two fixed 600-frame validations on the older downsampled sources pass every deadline and durability criterion. The later [supplied native-geometry GoPro test](stock-native12-gopro-timing.md) fails both fixed repeats with the same binary/profile. Native-input CPU margin and input/session robustness remain unresolved. The following chronology preserves earlier failures; those results are not the latest profile status.
   The [writeback follow-up](stock-capture-writeback.md) reaches 20.0047 durable
   fps with all 600 frames/previews exact, but 16 write/33 preview deadlines
   remain late. Poorer repeats and an unchanged-binary control also show
   unresolved platform timing variability; this is not a closed gate.
   The subsequent [preview-layout comparison](stock-preview-gain-layout.md)
   reduces retained unity-cap gain storage and measured rendering medians, but
   sustained repeats still overrun. A persistent-worker prototype was discarded.
   A subsequent CFA canonicalization prototype was likewise rejected after
   measurement; [retained independent preview coverage](stock-preview-register-experiment.md)
   now includes all four phases at 12/14/16 bits.
   Optional [compute-stage serialization](stock-preview-compute-stages.md)
   subsequently improves matched durable replay from 16.41–16.76 to
   18.28–18.63 fps, with exact accepted frames/previews. Both serial runs still
   overrun and miss deadlines; this is not closure of the timing gate.
   The following [direct final-plane reconstruction](stock-preview-direct-output.md)
   removes a measured copy and unused bands. Both matched serial runs preserve
   all 600 attempts at 19.9752/19.9988 durable fps and use 11.10 MB scratch,
   but hundreds of deliveries remain late. The gate is still unmet.
   Two [subsequent kernel proposals](stock-preview-rejected-kernels.md) were
   tested and discarded: bounded-token NEON dequantization lacks a consistent
   end-to-end win, and per-row addressing is slower in isolation. Runtime
   source is restored; their experimental test counts do not replace current
   validation or close the timing gate.
4. Keep sensor/DMA, rear-display, actual SD, thermal and physical A/V work
   explicitly unverified. The original PR did not establish those either.

This is broad implementation coverage, not proof of full goal completion while
broader quality and timing gates remain unresolved. Planning percentages
are not passing-test ratios.

## Latest checkpoint and timing evidence

The [published 1x native checks](stock-cleanup-native.md) pass FP32 Core ML and
Metal count budgets, with exact supported calibration. Faster FP16 Core ML
fails its preregistered precision bound. The [2x follow-ups](stock-sr-balanced.md)
fail shadow/highlight guards and supply no new weights. The current-binary
[background workload control](stock-background-control.md) preserves all 600
recordings/previews per run, but pausing competing work still leaves late
deliveries. None of these results closes broader quality or strict Pi timing.

The [region-balanced 2x follow-up](stock-sr-relative.md) also fails final guards.
Its full-frame validation diagnostic fails three of four held-out validation
sources despite patch-level success. This exposes a validation-coverage gap;
full-frame guards should precede subsequent final-reference evaluations.

The [full-image selector](stock-sr-full-selection.md) now implements this
validation stage, with nine optional CNN tests passing on both Mac and Pi.
Forty complete validation-image inferences reject all ten patch-qualified
candidates before further final evaluation. This closes the missing selection
mechanism, not the 2x model-quality gate.

Two [encoder-kernel proposals](stock-encoder-rejected-kernels.md) are tested
and discarded: paired 64-bit unpack loads preserve scalar results but are
slightly slower, and a precombined small-code pair table preserves byte streams
but is slower on its intended synthetic workload. Neither changes production
source or closes the strict Pi timing gate.

A [borrowed preview-plane handoff](stock-preview-plane-leases.md) removes the
queue's three-plane copy and passes ownership/output checks, but both matched
Pi lease runs have slower rendering and higher preview p95 than copying
controls. All 2,400 recordings/previews remain exact; the strict timing gate
still fails. The prototype is discarded, all six files are restored, fresh
26 Mac/23 Pi tests pass and the prior Pi executable is reproduced byte for byte.

The [source-wide 2x follow-up](stock-sr-source-regions.md) supplies an explicit
frozen diagnostic checkpoint after unchanged patch guards, full validation on
all 22 qualified combinations and four-reference CPU/MPS nonregression.
Pooled active gain is only +0.00729 dB; useful restored detail/tier/temporal
approval remains open. Both supplied 12 MP Pi inputs produce 50 MP editable
outputs with exact calibration and at most one-count CPU/MPS differences.
The separate 1x checkpoint and production encoder/capture source are unchanged;
[new 2x native artifacts](stock-sr-native.md) pass 12 execution and 24 full-reference
checks. Useful detail, broader quality and strict Pi timing remain open.

A [bounded shared unpack-row cache](stock-shared-rows.md) is rejected after
byte-exact isolated Pi comparisons: it computes each row once for all four
consumers, yet both eight-row comparisons are slower. Production runtime and
the unresolved strict deadline gate are unchanged.

[Early per-channel preview reconstruction](stock-channel-preview.md) also
preserves payloads/planes and passes the expanded Mac/Pi matrix, but neither
pinned nor affinity-restored isolated comparison provides a consistent win.
The prototype and draft API are discarded; production behavior is unchanged.

A [64-bit entropy chunk experiment](stock-entropy64.md) catches and corrects a
full-buffer boundary in its draft, then passes expanded Mac/Pi arithmetic/stream
proofs and the Pi matrix. The hybrid is about 1.3% faster on two standalone
real-input encodes, but both coupled runs have lower durable throughput than
both controls. All 2,400 recordings/previews remain exact. The proposal is
rejected; production runtime and the unmet strict deadline gate are unchanged.

The retained [bounded preview-plane spacing](stock-preview-colors.md) fits inside
existing reservations, with dense fallback at tight limits. Matched final
counters show about 5.8%/4.6% fewer L1 refills; cycles are flat on one input and
about 1.3% lower on the other. All 4,800 prototype/final recordings and previews
remain exact. Both final runs have fewer late writes/previews than both old
controls, but every run still fails the strict deadline gate. New source passes
27 Mac/24 Pi tests and four relocated package profiles.

Hosted `f332e07` initially fails native CTest because the explicit CI build
list omits the newly registered wavelet-queue executable. The [workflow
integration correction](stock-preview-colors.md#ci-target-integration) adds it
and retains failure logs; all seven hosted checks pass at correction `40aa476`
([receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/ci-integration/passed-40aa476.json)).
The local 27 Mac/24 Pi source checks and package evidence are unchanged.

The [exact packed gamma-table experiment](stock-packed-gamma.md) reduces the
lookup storage from 64 KiB to 8 KiB and passes exhaustive Mac/Pi comparisons.
It reduces cache refills but adds instructions; both full-capture render
medians are slower than both dense controls, and every strict timing gate
still fails. All 2,400 payloads and previews remain exact. The proposal is
rejected; production runtime is unchanged.

The [channel-balance diagnostic](stock-channel-balance.md) verifies three
120-frame instrumented replays and finds channel 0 usually finishes last in
the unrestricted case. Dispatch/join delays are small; sampled entropy coding
is the largest process-wide hot symbol. This supplies direction for finer
work-distribution investigation, not a production scheduling change or a
closed timing gate.

The [bounded entropy handoff](stock-entropy-handoff.md) implements helper
work-sharing with exact per-band state and fixed storage. Mac/Pi matrix,
concurrency and fallback checks pass, as do 2,400 captured payloads/previews.
Eight-row standalone comparisons are slower; 32-row chunks still fail to
establish a consistent benefit. Both prototypes are rejected and local tracked
source is restored. Production and the strict timing gate remain unchanged.
All seven hosted jobs pass at `a00ae4a`
([receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-balance-receipts/passed-a00ae4a.json)); later evidence
commits need their own hosted results.

A [current-executable preview priority control](stock-current-priority.md)
repeats the background/foreground comparison after direct-output and plane
spacing changes. All 2,400 attempts/payloads/previews are exact. The final
background control beats both foreground runs on late deliveries, so there is
no consistent priority benefit and defaults stay unchanged. Its p95 below
50 ms still includes 11 late writes/17 late previews; the strict gate is open.

Two [bulk zero-run scanners](stock-zero-scan.md) preserve all tested prefixes,
including guard boundaries, and all 492 independent real-input payloads, but
64-bit word and 128-bit NEON variants are slower than both scalar controls on
both inputs. Both are rejected without proceeding to capture timing. Prototype
source is restored. All seven hosted checks pass at `950deda`
([receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-current-priority-receipts/passed-950deda.json)); later evidence
commits require their own results. Production/model behavior is unchanged.

Two [latency diagnostics](stock-latency-trace.md) preserve all 1,198 accepted
payloads/previews from 1,200 attempts. A file-logged repeat still finds roughly
30 ms unaccounted critical-worker intervals with involuntary switches. These
are instrumented attribution results, not acceptance timing. A subsequent
[uninstrumented affinity rotation](stock-affinity-offset.md) preserves all
2,400 attempts exactly but does not consistently improve timing, so the
proposal is rejected. The best default control still misses four writes/seven
previews. Production and unresolved quality/timing gates are unchanged. All
seven hosted checks pass at `6efc5bd`
([receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/passed-6efc5bd.json)); newer evidence
commits require their own hosted results.

[Kernel scheduler/workqueue attribution](stock-kernel-scheduler.md) identifies
`mmc_rescan` occupying CPU 0 in the largest corroborated gaps, with zero trace
loss/continuity mismatches. The [explicit scheduling profile](stock-process-scheduling.md)
then reproduces two 600-frame FIFO-priority-1/foreground-preview validation
passes at 20.0056/20.0065 durable fps, all attempted payloads/previews exact and
zero late deliveries. An earlier same-profile run still fails (4 late writes,
250 late previews), so this is a scoped floor demonstration, not blanket
reproducibility or default-scheduler approval. No boot/device/global scheduling
setting, codec source or model is changed. All seven hosted jobs pass at
`cade228` ([receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/passed-cade228.json)); new evidence
heads need their own checks. Broader timing and model-quality gates remain open.

The [CNN tactics audit](stock-cnn-tactics.md) checks the old selected preclean/detail/coordinate recipe, structure losses and checkpoint interpolation against primary research. A new validation-only spatial gate improves full-validation gain from +0.02782 to +0.08716 dB without regional regressions. It is frozen for further evaluation, has no final/native approval and does not replace the supplied model. Reproducing the specialized older training recipe remains an explicit quality task.

The frozen gate passes four historical references on CPU/MPS (+0.022879 dB pooled, at most one count cross-host), but remains analysis-only without useful-detail/native/Pi approval. Separately, the [eight-case older-recipe comparison](stock-sr-recipe-comparison.md) completes training and 72 full-image inferences; all 18 shortlisted candidates fail regional guards. No replacement model is promoted; broader quality remains open.

The complete recipe-family follow-up evaluates all 48 patch-qualified settings across four full validation images: ten pass, but best gain is +0.00735 dB versus existing BIBO +0.02782 dB. The earlier shortlist missed weaker passing candidates. No quality replacement or final-reference promotion follows; the shadow overshoot and full-family receipts are retained.

The [historical-width/pretraining comparison](stock-sr-historical-recipe.md)
now reconstructs the 633,444-parameter old selected architecture and recorded
structure losses on stock pairs. Starting from the old weights gains +0.97659 dB
on patches versus -0.76376 dB from scratch under the same 400-update budget.
Training-only shadow constraints yield patch-qualified families with up to
+0.57218 dB pooled full-image gain, but all fail complete-image regional guards.
The audit covers 2,400 updates and every qualified candidate across four images
(40 inferences). Border artifacts and interior shadow overshoot both remain;
no new weights or quality approval are promoted.

The [supplied-camera study](stock-cnn-gopro.md) closes a concrete quality-evidence
gap: the exact supplied 1x checkpoint passes four RAW regions on each of two
actual 12 MP GoPro files, and the 2x diagnostic passes one 50 MP GPR-derived
reference simulation. Matched 16-bit rendered RGB/luma fidelity also improves
on all three. New shared full-frame GainMap simulation support passes 23
focused Mac/Pi test methods and exact roundtrips for all three actual camera
metadata packets. Original noise, lost texture, broader/temporal quality and
restored-tier promotion remain unresolved; decoded compressed references are
not sensor truth. No weights or native encoder source change.

A separate [four-case signal-conditioning study](stock-sr-signal-conditioning.md)
improves full validation but fails six of 32 historical-reference regional
checks and is not promoted. A [direct transfer](stock-old-sr-transfer.md) of the
exact old selected SR checkpoint loses 5.36 dB on the current stock test, while
a matched old-input control reproduces its original 60.30149 dB. The older
weights are not a drop-in stock substitute. [Mission 1 source grouping](stock-sr-mission-sources.md)
binds 42 cached RAW references and a conservative future scene split; original
GPRs are unavailable for fresh redecoding and per-frame calibration remains to
bind before new training claims. The old PRs remain untouched.

The [horizontal inverse SIMD follow-up](stock-horizontal-inverse.md) retains
a new actual-input user-cycle profile and two exact SIMD proposals. Rounded
shift-and-accumulate improves the isolated Pi helper by 9.05%, but native
combined ABBA remains inconsistent and every run fails the strict floor. The
proposal is discarded; production hashes are unchanged. This is evidence for
further combined-path investigation, not a new runtime optimization or closure.

The [retention-cost follow-up](stock-preview-retention.md) isolates a 6.7–7.0 ms
cost before reconstruction on supplied native 12 MP counts. Three storage
proposals preserve bytes/pixels but fail to establish a consistent workflow
benefit across native and older controls. All are discarded; runtime source
is restored and the Pi executable again matches the original complete hash.
Fresh 27 Mac/24 Pi restoration tests pass. No new optimization, quality approval
or realtime floor is claimed.

The [historical boundary/input-gate study](stock-sr-historical-boundary.md) now qualifies a frozen RAW-fidelity diagnostic: +0.53917 dB full validation, +0.12038 dB on historical references, and +1.13707 dB on GP011689. All declared regional and one-count CPU/MPS guards pass, with supported editable calibration verified. However, rendered RGB gains only +0.00373 dB versus supplied BIBO +0.28430 dB; sensor black-level/gate coverage differences remain. This does not replace the supplied weights or qualify visible detail, temporal behavior or native/Pi execution.

The [calibrated camera-pair study](stock-sr-calibrated-mixed.md) verifies 1,088 source/stock-decode patch pairs using recovered archival metadata and preserves the existing venue split. Four matched training cases fail Nikon shadows and do not advance. It also fixes actual 65x65 GainMap parameter import: capacity-matched storage and JSON cleanup, with exact pixel/opcode regressions at 9x9/65x65/97x97 and three focused SDK/raw/still CTests passing on both Mac and Pi. Capture/preview code and Pi capture executable are unchanged.

The [sensor-gate and frozen-cascade follow-ups](stock-cnn-followups.md) complete the bounded comparisons without a replacement: 18 patch settings, 20 full gate outputs, ten complete cascade outputs and actual rendered comparisons. The only gate survivor is worse than supplied BIBO in pooled RGB; the cascade improves rendered Nikon fidelity but regresses every camera validation image. No thresholds or failed guards are relaxed. A separate [JSON boundary correction](stock-sdk-parameter-json.md) fixes the specific new hosted parameter-import failure, with fresh focused Mac/Pi passes.

The [matched joint-training follow-up](stock-cnn-joint.md) completes 1,200
updates across SR-only, frozen-cleanup and joint-cleanup arms. Joint training
has the largest pooled patch gain (+0.14394 dB versus supplied SR), but every
checkpoint fails the unchanged Nikon shadow guard. All nine control/candidate
settings are independently re-inferred and rescored; no model is selected or
advanced. Broader quality and native combined timing remain open.

The [upper-band recomputation study](stock-preview-recompute.md) rejects two
payload/coefficient/plane-exact proposals after matched isolated Pi timing:
both add 2.84–3.53 ms to retained-band reconstruction. A separate allocation-only
control stays within about 0.1 ms of ordinary compact encoding despite matching
the retained allocation extent. This narrows the material cost to retained-band
processing rather than reservation alone. All 1,664 payloads and the 1,536
actually reconstructed planes verify; no sustained capture test or timing
gate closes, and production source/binary hashes are unchanged.

## Streaming-row integration

The [optional streaming preview](stock-wavelet-stream-prototype.md) is implemented,
with new public row callbacks, a bounded inverse consumer and capture lifecycle
integration. Fresh Release checks pass 29 Mac/23 SDK-less Pi CTests, eight frontend
tests on each host and four relocated Mac package profiles. In the fixed native
Pi ABBA comparison, both streamed runs record/preview all 600 frames exactly;
controls record 548/599. The streamed runs retain 154/44 late writes and 171/47
late previews at 19.9965/19.9980 durable fps. The strict combined gate remains
open. No CNN weights or their quality scopes change. All seven hosted jobs pass
at source/test revision `3b2c0f0` in
[run 41184771455](https://github.com/dcliftreaves/gpr/actions/runs/41184771455).

The [post-streaming profile and controls](stock-stream-followups.md) reject
progressive color, the existing full-frame unpack fallback, trailing-zero
batching and two shared-green cache layouts. Exact output checks and compact
source-bound receipts remain available. None changes runtime or closes the
strict native-input deadline gate.

## Current supplied-checkpoint integration audit

The original #95/#96 scope and adoption map were re-read alongside current
code/tests and receipts. Old fork #95/#96 and upstream #71/#72 remain open at
their frozen heads; #101 remains a separate draft based on #100/#99. All seven
hosted jobs pass at evidence revision b09a6cb.

The audit identifies and closes a concrete validation gap: the previously
complete native export chain used an earlier checkpoint. A [fresh streaming
recording with the supplied 2x model](stock-selected-video-chain.md) now passes
independent pixels/calibration, matched native/Torch counts, historical Q5
payloads, exact GVID/MOV recovery and actual-timing 8K ProRes checks. It also
corrects missing native reconstruction settings in ProRes receipts, with a
fresh contract pass. All seven hosted jobs pass for that receipt fix at 589bc29 in
[run 41188343975](https://github.com/dcliftreaves/gpr/actions/runs/41188343975). This two-frame chain does not close strict sustained Pi deadlines,
broader useful-detail/restored-tier quality, temporal or physical camera gates.

The fresh-recording selected-model CPU follow-up on Pi also passes its existing
one-count Mac MPS agreement bound, with independent pixels/calibration checks
and 97.996/98.949 seconds of offline inference per frame. This extends the
current chain's input-specific execution evidence; the strict recording
floor and broader model/temporal acceptance remain open.

The [prepared streaming coefficient lookup](stock-stream-dequantization.md)
removes the per-row inverse-companding pass without changing payloads, pixels,
public APIs or workspace. Fresh native Pi ABBA candidate runs accept/preview
600/600 each, with median encode 34.00/34.93 ms versus baseline 37.99/37.16 ms.
All 2,370 accepted frames are independently verified. Fresh Mac/Pi CTests,
frontend checks and four relocated Mac package profiles pass. Late deliveries
and durable fps below 20 keep the strict combined gate open. No CNN weights or
quality scopes change.

The [stock weight-interpolation comparison](stock-cnn-interpolation.md)
recreates another historical selection tactic with related current SR
checkpoints. All ten fixed blends fail Nikon shadow guards despite pooled
improvements. Independent checks bind 1,530 parameter tensors and reproduce
all twelve prediction/score controls. No model, inference backend or quality
approval changes; useful broad 2x and temporal quality remain open.

All seven hosted jobs pass for the prepared-lookup source revision `654775b`
in [run 41191796628](https://github.com/dcliftreaves/gpr/actions/runs/41191796628),
including native backends, FFmpeg and relocated packages. The subsequent
weight-interpolation report adds evidence only; runtime and supplied models
remain that validated source.

The [quantizer-specific preview tables](stock-stream-quant-tables.md) add
73,692 accounted bytes and reduce the final same-process encoder/inverse
medians by 4.34%/3.62%, with every candidate block faster on both native sources.
Final Mac/Pi/full-package checks and exact-capacity/preset-change tests pass.
Two final 600-frame captures are exact but retain 1/22 late writes, 3/20 late
previews and 19.9964/19.9987 durable fps; the strict combined floor stays open.

The [CNN translation diagnostic](stock-cnn-translation.md) adds a synthetic
spatial-consistency check, with independent transform/scoring and CPU controls.
Its measured sampling-phase sensitivity supplies a concrete next model target;
actual motion/rendered/temporal and restored-tier quality remain unapproved.

The [preview scheduling study](stock-preview-scheduling.md) identifies a late
fourth color task, rejects three fixed partitions and adopts four workers
sharing 32-row blocks. All 2,400 candidate/control frames are exact. Candidate
runs retain 1/15 late previews versus controls 24/30, at 19.9969/19.9977 durable
fps; the strict combined gate remains open. Fresh 30 Mac/24 Pi Release CTests,
eight frontend checks per host and all four relocated Mac package profiles pass.

The [phase-training study](stock-cnn-phase-training.md) tests three matched
600-update arms on actual stock inputs. Phase MSE improves, but every candidate
fails RAW guards, so supplied weights and quality scopes remain unchanged.
No whole-image/rendered/final/backend/Pi approval is inferred for those models.
The preceding published source `eb3062b` passes all seven hosted jobs in
[run 41193609961](https://github.com/dcliftreaves/gpr/actions/runs/41193609961).
New row-scheduling source requires its own hosted result.
