# Original feature and validation accounting

Audit sources: fork PR #95 runtime `8be29eee57b4637865b572f18ab9e3f4edaa2236`,
PR #96 validation `821420d7acac8a316d16293b93d4b3204631a38f`, their PR bodies,
`docs/ADOPTION.md` and `tests/Validation.cmake` at those revisions. The original
eight adoption groups define the rows below. This is an accounting of current
implementation and evidence, not a claim that all original features are done.

| Original group | Current stock implementation/evidence | Remaining or different |
|---|---|---|
| Stills | Four CFA phases, 12/14/16-bit words, packed-12 SDK paths, six presets, typed calibration and editable DNG/GPR; `stock_still`, `bayer_formats`, `raw_media`; [format contract](stock-still-formats.md) and [quality evidence](stock-still-quality.md). One real 11664x8750 X2D source passes all six presets with three repeats and independently verified source decoding | Old compact three-tier size/quality approvals do not transfer. New restored tiers lack approved models. The single 100 MP source does not establish a broad camera corpus. |
| Fused codec | Replaced by the requested historical stock-compatible VC-5 architecture. Exact still/replay payload checks and encoder/decoder boundary tests cover the replacement. [Stock encoder](stock-encoder-sub100.md) | Fused subband/ANS edge tests cannot prove stock VC-5 behavior and are not applicable. Old Q8/profile semantics are deliberately not advertised. |
| Capture/GVID | Bounded copy/lease ownership, asynchronous writer, flush/finish/cancel, actual PTS, source metadata, CRC/END3 and explicit prefix recovery; `capture_lifecycle`, `capture_c_api`, `gvid3_conformance`, `gvid2_compatibility`; [contract](stock-capture-api.md) | Old fused GVID is not silently interpreted as stock GVID. Physical sensor/storage scheduling and durable SD remain unverified. |
| Native preview | Independent stock RGB fixtures, calibrated source plans, all reductions/orientations, bounded latest-pending queue and sink-error isolation. Optional encoder-derived quarter-scale path eliminates entropy decoding; [Pi receipts](stock-capture-preview.md) | Physical display, broader lens/look/temporal approval and strict sustained zero-late timing remain open. Encoder path is opt-in and quarter-scale only. |
| Desktop outputs | Exact GVID/MOV recovery, >4 GiB offsets, PCM/timecode, editable Bayer export and calibrated macOS deep ProRes with optional reconstruction. Core ML/MPSGraph/hybrid engines now also run through a standalone native raw executable, with standard-library still/GVID/timelapse/ProRes control paths; [native runtime](stock-native-runtime.md), [Metal](stock-metal.md), [Core ML](stock-coreml.md), [MOV](stock-mov.md), [ProRes](stock-prores.md) | Historical monolithic command syntax is not retained; containers are coordinated by the standard-library layer. Native simulated-capture/regression analysis remains on the analysis workflows. Backend/host arithmetic is not universally bit-identical to Torch. Selected export interoperability and look/motion approval remain open. |
| Optional FFmpeg decoder | New `gpr_stock` adapter for pinned FFmpeg n8.0 directly decodes GVS2/GVS3 to Bayer, using checked clip configuration in `glbl`. Twelve CFA/depth combinations, frozen fixtures, corruption rejection and PNG/ProRes exercise the replacement; [build/contract](stock-ffmpeg-decoder.md) | Requires a patched FFmpeg build; old MOVs without decoder configuration must be unwrapped/rewrapped. Native/hosted platform results are revision-bound; this is not bundled-application support or calibrated color rendering. |
| Reconstruction | Explicit portable 1x/2x architectures, checkpoint contracts, editable sequence/still output, optional GPR re-encoding and fresh source-grouped training/evaluation. Stock UPRESABLE provides timelapse/regression/both orchestration, direct captured-GPR inputs, exact GPR sequence packing and optional calibrated ProRes. Two real 12 MP Pi inputs produce independently verified 8192x6144 outputs. Core ML/MPSGraph/hybrid execution and a standalone native host cover the supported media paths; [workflow/measurements](stock-upres-workflow.md), [native runtime](stock-native-runtime.md), [portable runtime](stock-cnn-portable.md), [cleanup experiment](stock-cleanup-training.md), [SR experiment](stock-sr-training.md) | The separately frozen half-strength 1x candidate passes the declared four-reference numeric guard and is supplied explicitly. Earlier 2x candidates regress in shadows. The [later source-wide diagnostic](stock-sr-source-regions.md) passes declared CPU/MPS numeric guards and is supplied explicitly, but gain is small; broader model/tier/temporal promotion remains open. Broader real-source and temporal/motion quality remain open. Native simulation/regression analysis uses the existing analysis workflows; historical command syntax and universal cross-backend bit identity are not claimed. |
| Integration/validation | Separate fork draft #101, four stripped/relocated packages, external C/C++ clients, optional reconstruction contracts, Mac/Pi Release suites and seven hosted platform jobs | Current feature checkpoints use the new architecture rather than replaying old eight-commit adoption identities. Latest CI must pass for each head. Broader fuzz/sanitizer expansion is explicitly deferred at the user's request. |

The old PR explicitly left firmware deployment, sensor/display integration,
durable SD, external model weights and premium still-SR approval unproven.
Those boundaries remain explicit; preserving an old limitation does not make
it completed hardware or model-quality validation.

Mac and Pi functional validation of the new FFmpeg decoder passes, with its
hosted platform gate passing at `eaeaefa`. New command orchestration also passes
on the two real captured-GPR Pi sources. Native backend execution is implemented;
it is no longer an unimplemented parity item. Reuse of an old model or GPU
backend still requires a stock-input quality check. No old
visual/quality approval, size-tier claim, or historical timing receipt is
relabeled as new evidence.

## Applicable validation from #96

The frozen `tests/Validation.cmake` registers the tests in the first column.
Replacement tests check corresponding stock contracts; old fused-codec tests
cannot establish correctness of a different codec architecture.

| Original validation | Current evidence | Replacement scope |
|---|---|---|
| `test_labs_encoder_api`, `test_video_encoder_abort`, `test_video_full_chain`, portable pipeline simulation | `capture_lifecycle`, `capture_c_api`, `capture_preview`, `stock_sequence`; real Mac/Pi replay | Copy/lease ownership, bounded queues, cancellation waking blocked callers, flush/finish/writer errors and recording-to-preview/export. Replay uses supplied frames, not a sensor. |
| `gvid_format` | `gvid3_conformance`, `gvid2_compatibility`, `video_format_matrix`, `stock_mov` | Stock profile/version checks, CRC/END3, metadata, truncation and explicit prefix recovery. Old fused GVID is not silently accepted. |
| `fused_band_roundtrip`, `fused_pool_drop_hp`, `fused_edges_split`, `fused_edges_inline`, `entropy_inline_tail_flush` | `bayer_formats`, `bayer_formats_fast`, `encoder_bounds`, `encoder_bounds_fast`, `decoder_bounds`, encoder-preview probes and historical payload/pixel comparisons | Applicable boundary/equivalence checks rebuilt for stock VC-5. Fused pool, split/inline joint-ANS and tail-flush details are replaced, not ported. |
| `still_metadata` | `sdk_c_api`, `raw_media`, `stock_still`, independent real-source still receipts | Typed supported metadata, editable counts, twelve CFA/depth combinations, six presets, SDK ownership and real Nikon/X2D sources. Unsupported metadata stays explicit. |
| `native_preview` | `preview_contract`, `preview_scalar_contract`, `preview_queue`, `preview_cli`, `stock_preview_profiles`, `capture_preview` | Independent RGB fixtures, calibration/orientation/reductions, serial/vector equivalence, queue ownership and failure isolation. No physical display claim. |
| `sr_runtime`, registry/model contracts and external quality gates | `stock_cnn_test.py`, `stock_upres_test.py`, `stock_coreml_test.py`, `stock_metal_test.py`, `stock_native_test.py`; fresh 1x/2x training/evaluation | Checkpoint/artifact binding, scales/tiling/count domains, native engines, editable output and end-to-end workflows. Earlier full-strength 1x and 2x failures remain recorded. Later 1x and source-wide 2x candidates pass their declared numeric guards; the latter has small benefit and broader approval remains open. |
| Desktop/FFmpeg integration | `stock_mov_test.py`, `stock_prores_test.py`, `stock_ffmpeg_test.py`; real A/V, >4 GiB and native media receipts | Exact payload/timing recovery, declared PCM/timecode, calibrated deep ProRes and optional pinned FFmpeg Bayer decoder. Editor interoperability remains broader than these checks. |
| Adoption/build/CI/evidence tooling | `build_profiles.py`, relocated external C/C++ clients, seven hosted jobs and revision-bound receipts | Current independent packages replace old eight-commit adoption identities. Old feature-coverage instrumentation/research diagnostics are not reproduced. Broader fuzz/sanitizer expansion is deferred at the user's request. |

## Delivery versus approval

All eight original production groups have a stock implementation or an explicit
architectural replacement above. This establishes feature coverage, not
completion of the entire goal or approval of every output. The
[requirement ledger](stock-rebuild-requirements.md) distinguishes implementation,
measured failures, unsupported combinations and external validation. Two later [fixed-profile Pi validations](stock-process-scheduling.md) meet the strict
20 fps recording/preview floor; default scheduling and broader repeatability
remain unresolved, as does broader/2x model-quality approval;
the later [half-strength 1x study](stock-cleanup-strength.md) passes its declared
validation and four-reference numeric guards.
Percentages are planning estimates, not fractions of passing tests.

The [writeback follow-up](stock-capture-writeback.md) adds an optional Linux
storage policy. Its final 600-frame replay reaches 20.0047 durable fps and
retains all pixels/payloads, but misses 16 write/33 preview deadlines. Slower
repeats, including an unchanged-binary control, expose additional unresolved
platform timing variability. No sustained-floor approval follows.

The [gain-layout follow-up](stock-preview-gain-layout.md) retains exact unity-cap
compaction and large-frame serial/parallel coverage. Persistent-worker reuse was
tested and discarded for lack of consistent benefit. Latest sustained repeats
still overrun, including a control with the orphaned background benchmark paused;
the strict timing gate remains unmet.

The next [register experiment](stock-preview-register-experiment.md) was also
discarded after a slower isolated renderer and no consistent sustained benefit.
The retained validation now covers all twelve CFA/depth combinations and 576
calibrated configurations, with runtime behavior unchanged.

The [compute-stage follow-up](stock-preview-compute-stages.md) adds an opt-in
wait after writer enqueue, allowing storage/preview overlap while preventing
preview from competing with the next encode. Same-executable ABBA replay
improves durable fps from 16.41–16.76 to 18.28–18.63 with exact accepted output.
Overruns and late deliveries persist; the strict timing gate remains unmet.

The [direct final-plane follow-up](stock-preview-direct-output.md) removes a
profiled reconstruction copy and unused final bands. Both matched serial runs
retain all 600 frames/previews at 19.9752/19.9988 durable fps, with exact output
and lower used scratch. Late writes/previews remain; this is not gate closure.

Later [dequantization and row-address proposals](stock-preview-rejected-kernels.md)
pass their numeric checks but fail to establish a consistent workflow gain;
isolated row addressing is slower. Both are discarded and their sources,
measurements and restoration identities are retained separately.

## Latest evidence follow-up

The [published 1x checkpoint](stock-cleanup-native.md) passes checkpoint-specific
FP32 Core ML and Metal checks on two supplied 12 MP sources through standalone
standard-library workflows; faster FP16 Core ML fails its precision budget.
The [2x strength/retraining study](stock-sr-balanced.md) still fails regional
quality guards, so no new 2x weights are promoted. A current-binary
[Pi workload control](stock-background-control.md) preserves every recording
and preview but still misses deadlines with competing processes paused.
All seven hosted jobs pass at source/model revision `c47fdf1`; these new reports
and receipts do not change production source or model weights.

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
checks. Broader quality and strict Pi timing remain open.

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

The retained [bounded preview-plane spacing](stock-preview-colors.md) changes
internal placement, with exact pixels/payloads, existing memory limits and dense
fallback preserved. Final source passes 27 Mac/24 Pi tests and four relocated
profiles. Lower measured cache refills do not close the strict deadline gate.

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

The [actual supplied-GoPro study](stock-cnn-gopro.md) now supplies positive
Q3 RAW and matched 16-bit rendered fidelity evidence for both frozen models,
with exact supported calibration and focused Mac/Pi verification. It extends
source-specific evidence without approving original-noise removal, useful
recovered detail, temporal behavior or old restored tiers. The [exact older
selected SR transfer](stock-old-sr-transfer.md) fails on current stock input;
one matched old-profile control reproduces its recorded result. New
[signal conditioning](stock-sr-signal-conditioning.md) also fails final regional
guards. Existing checkpoints remain unchanged; further source-domain coverage
can use the explicitly bounded [Mission 1 source audit](stock-sr-mission-sources.md).
