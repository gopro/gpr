# Rebuild the RAW-media feature suite on the stock encoder

Start with the [feature overview](stock-raw-media-overview.md) for user workflows,
current scope and the finish line, or the [review plan](stock-review-plan.md) for
the proposed commit sequence. This page retains build commands and the
historical integration record.

## Objective and source revisions

Recreate the high-level workflows of fork PR #95 / upstream PR #71 using
the stock-compatible encoder and RAW-video experiment as the foundation.
Work in a separate fork branch and replacement draft PR. Keep the existing
feature and validation PRs open; do not retarget, merge or close them.

- Previous runtime: `8be29eee57b4637865b572f18ab9e3f4edaa2236`
  (`review/product-runtime`, fork #95, upstream #71).
- Previous validation: `821420d7acac8a316d16293b93d4b3204631a38f`
  (`review/product-validation`, fork #96, upstream #72).
- New foundation: `d3d96ec49970a7edc8736d07ed39b3964ca4dc10`
  (`experiment/stock-raw-video-12mp`, fork #100), including fork #99.
- Rebuild branch: `feature/stock-raw-media-runtime`.

The old suite's approved results describe its old codec/model pairings.
Historical evidence remains historical. Porting an interface or model loader
does not establish parity, quality, performance, or camera deployment.

Generated receipts are now retained in a [verified evidence archive](stock-evidence/README.md).
Guides link to readable immutable copies; local and installed packages can
restore the original paths without downloading anything.

## Current delivery priority (2026-10-29)

Finish a usable end-to-end path before expanding support at every stage.
RAW stills retain broad Bayer/count-format support. RAW video delivery focuses
on the Mission 1 workflow: 4096x3072 recording, calibrated preview, editable
RAW extraction and desktop ProRes, with explicit optional offline 1x/2x
reconstruction. The existing working stock RGGB14 path is the immediate
integration target; additional video/render/reconstruction format combinations
are stretch work, not prerequisites for this delivery.

The earlier Mission 1 handoff specifies a 20 fps floor for 4096x3072 capture
and 1024x768 preview. Retain that floor separately from the more demanding
30 fps replay experiments. A host replay and memory preview sink cannot close
the actual sensor/DMA, rear-display, SD durability or camera thermal gates.
Prioritize ordinary Release integration, real-sample workflows and reviewable
local packages. Broader sanitizer/fuzz auditing is deferred from the immediate
critical path. The user authorized source uploads and pushes to their Git fork
on 2026-10-30; the replacement draft is fork PR #101. Existing feature and
validation PRs stay open.

## Core workflow

Build the full desktop workflow on macOS with FFmpeg development libraries,
Python, NumPy and tifffile available. Capture/preview library profiles remain
independent of the desktop SDK and Apple renderer.

```sh
cmake -S . -B build-runtime -DCMAKE_BUILD_TYPE=Release \
  -DGPR_ENABLE_VIDEO=ON -DGPR_BUILD_CAPTURE_APP=ON \
  -DGPR_BUILD_PREVIEW_TOOLS=ON -DGPR_BUILD_PREVIEW_TESTS=ON \
  -DGPR_BUILD_STOCK_VIDEO_EXPERIMENT=ON -DGPR_BUILD_MEDIA_TESTS=ON \
  -DGPR_BUILD_CODEC_TESTS=ON -DGPR_BUILD_VIDEO_TESTS=ON \
  -DGPR_BUILD_MOV_TOOLS=ON -DGPR_BUILD_MOV_TESTS=ON \
  -DGPR_BUILD_RENDER_TOOLS=ON -DGPR_BUILD_RENDER_TESTS=ON \
  -DGPR_BUILD_PACKAGE=ON
cmake --build build-runtime --parallel 4
ctest --test-dir build-runtime --output-on-failure

# Calibrated still: declare the source's sample domain, not its storage word size.
python3 tools/stock_still.py encode source.dng --source-type dng --sample-bits 14 \
  --quality 3 --sdk build-runtime/source/app/gpr_tools/gpr_tools \
  --output still.gpr --editable still.dng

# Mission 1-style replay: tightly packed 4096x3072 RGGB14 words and actual calibration.
build-runtime/gpr_capture_app --input frame.raw --source-metadata calibration.json \
  --output recording.gvid --quality 3 --fps-num 20 --fps-den 1 \
  --frames 120 --paced --leased
python3 tools/stock_preview.py recording.gvid --output-dir preview-profiles \
  --compiler build-runtime/gpr_preview_profile --downsample 4
build-runtime/gpr_preview_app --input recording.gvid --profile-dir preview-profiles

# Recover editable RAW and wrap the original compressed recording without image conversion.
python3 tools/stock_video.py extract recording.gvid --out-dir editable \
  --decoder build-runtime/source/app/stock_video/stock_video --editable both
python3 tools/stock_mov.py wrap recording.gvid --bridge build-runtime/gpr_mov \
  --output stock.mov
python3 tools/stock_prores.py stock.mov --mov-bridge build-runtime/gpr_mov \
  --output rendered.mov --decoder build-runtime/source/app/stock_video/stock_video \
  --renderer build-runtime/gpr_render --encoder build-runtime/gpr_prores \
  --ffprobe /path/to/ffprobe --profile hq
```

All output names/directories must be new. These commands write local files;
"publication" in the tool receipts means exclusive local filesystem creation.
The preview command above is playback. To exercise recording and calibrated
preview simultaneously, use the repeatable matched replay:

```sh
python3 tools/benchmark_mission1.py \
  --capture build-runtime/gpr_capture_app --compiler build-runtime/gpr_preview_profile \
  --player build-runtime/gpr_preview_app --reference /path/to/independent/vc5_encoder_app \
  --input frame.raw --metadata calibration.json --frames 600 --fps 20 \
  --quality 3 --downsample 4 --out-dir new-mission1-review --require-floor
```

Add `--preview-from-encoder` to select the new quarter-scale retained-band
preview. On Pi this reduces current capture-to-sink p95 from 96.50 ms to
46.07 ms in a 600-frame replay, with all frames/pixels exact. Four late previews,
five late writes and final storage flush still fail the strict sustained gate;
`--require-floor` correctly returns status 2. See the
[current Pi receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoder-preview-600.json).

The reference app must honor `GPR_BENCH_QUALITY`; the historical codec may be
instrumented at the sample-app boundary without changing its encoder. Repeat
`--input` and `--metadata` in matching source order for alternating inputs.
The runner checks every accepted payload against independent stock bytes,
source calibration and attempt association, END3/accounting, and every
delivered preview against separately prepared offline playback. It reports
recording deadlines, preview deadlines and drops independently. Missing the
requested timing floor is not mistaken for a functional pass; `--require-floor`
returns status 2 for that case. Other formats are outside this focused runner.

### Local core review, 2026-10-29

The M5 Max replay uses two supplied camera-still-derived RAWs, warm cache and
a memory CRC preview sink. Both recording-only and combined runs accept/write
120/120 frames at a requested 20 fps, with zero overruns or late writes. All
240 payloads match the independent stock encoder. The combined run delivers
all 120 full-frame 1024x768 previews, with no replacements/discards and every
delivery within 50 ms of its replay timestamp. Durable recording throughput
is 20.12 fps; capture-to-preview p95 is 31.93 ms. This six-second host replay
does not establish sustained Pi or actual camera performance, nor a preview
speed benefit from the differing single-run encode medians.

Two lossless source stills (45.7 MP Nikon RGGB14 and 102.1 MP X2D RGGB16) pass
36 fresh encode/decode pairs across six presets/three repeats. Independent
lossless source decoding, every historical payload/pixel hash and every
distortion metric agree exactly. Q3 SDK-process/preparation medians are
32.61/96.33 ms and 67.19/148.99 ms respectively, before final publication/fsync.
These are scene-specific host workflow timings, not codec-only or camera latency.

All four relocated local package profiles pass. The full installed package's
capture, preview/compiler and SDK binaries have exactly the hashes used by
these measurements. The configured local Release suite passes 21/21; seven
optional CPU/Torch workflow contracts pass without trained weights. Exact
receipts: [Mission 1 replay](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-mac.json),
[stills](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/stills-mac.json),
[packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/packages-mac.json),
[Release checks](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/release-tests.txt),
[optional CNN contracts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/cnn-contracts.txt) and
[source/reference identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/validation.json).

Using that relocated full package, the actual 120-frame recording passes an
exact GVID/MOV/GVID round trip. Two exported editable DNG/GPR frames retain
their exact source calibration and agree pixel-for-pixel with the historical
independent decoder and the SDK. The complete private MOV then renders to
4096x3072 ProRes HQ: all 120 frames pass timing/color/profile checks and a full
independent FFmpeg decode. The single functional export takes 61.22 s before
publication and produces 682,542,583 bytes; this is offline processing, not
real-time ProRes. [Export receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/exports-mac.json).

The subsequent [streaming ProRes follow-up](stock-prores.md#bounded-image-scratch-2026-10-29)
removes the accumulated TIFF pool. The same 120-frame clip now peaks at
100.67 MB of TIFF storage / 154.71 MB of image scratch, with exact prior
ProRes packets, decoded pixels and timestamps. Ten Release rendering tests
pass. This closes the frame-image scratch growth gap without broadening the
focused RGGB14 video format scope.

The local review bundle is at
`/Users/dcliftreaves/Documents/slop/gpr_check/releases/stock-core-20261029`.
Its recording alternates two stills; no scene motion or temporal quality is
established. The bundle is a review candidate for the core host workflow.
Camera integration, Pi combined-preview performance, model quality approval
and broader per-stage format parity remain separate work. Optional desktop
PCM/timecode transport and ProRes inheritance are now implemented under the
[explicit auxiliary-track contract](stock-mov.md#optional-pcm-and-timecode).

### Earlier Pi 5 combined replay, 2026-10-30

A fresh replay from local revision `b3d9a0a` uses two existing 4096x3072
GoPro-derived RAW frames on the Pi 5, the performance governor, the optimized
stock encoder, calibrated preview and an independently built stock encoder.
Both 120-frame recording-only runs and both combined runs write all 120
frames with no overruns; every payload matches the independent stock encoder,
and every delivered preview matches separately prepared offline pixels.

That revision's requested 1024x768 preview did not meet the 20 fps handoff
floor. It delivered 56/120 frames, replacing 64 pending frames; median render
time was 105.11 ms and capture-to-sink p95 was 185.88 ms.

The current decoder can decode the three independent reduced GS/RG/BG VC-5
channels concurrently. The calibrated NEON display kernel consumes those
host-endian wavelet planes directly, fusing the historical WaveletToRGB pass,
byte/lane reversal, calibration, matrix and RGB8 conversion. A serial decoder
switch remains available for exact comparison. On the same Pi/source/quality
contract, the current path delivers **120/120** 1024x768 previews with no
replacement, a **49.01 ms** render median and exact offline preview pixels.
This is a 2.14x median render improvement over the 105.11 ms baseline, and it
closes the sustained-preview throughput gap.

It does not close the stricter end-to-end latency or durability gates.
Preview currently starts from the completed compressed payload, after a
26.92 ms median encode; capture-to-sink p95 is 99.18 ms and every delivery is
therefore later than the same-frame 50 ms deadline. A 701.37 ms final SSD
fsync lowers the short-run durable average to 17.97 fps despite zero capture
deadline misses and 20.07 buffered fps. Reusing encoder wavelet state (or a
raw-source preview path) is the next architectural step for same-frame latency;
changing final-flush accounting is not a codec optimization.

At 512x384 the same combined path delivers 120/120 previews with no
replacement, a 16.30 ms render median and 46.83 ms capture-to-sink p95. One
delivery exceeds the strict 50 ms deadline, and the 699.85 ms final fsync
again prevents the durable-average gate from passing. This is a useful
reduced-preview fallback, not evidence that the 1024x768 requirement is met.
The runs use a memory CRC sink and do not establish sensor, display, card or
thermal behavior. Exact receipts: [current parallel/fused 1024x768](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-parallel-preview.json),
[prior 1024x768 baseline](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-s4.json) and
[512x384 baseline](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-s8.json).

## Current task and evidence matrix

The [original eight-group parity audit](stock-feature-parity-audit.md) maps the
frozen #95/#96 adoption groups to this implementation. It explicitly identifies
the historical monolithic command syntax as an architectural difference.
[Standalone native reconstruction](stock-native-runtime.md) now runs Core ML,
MPSGraph and custom Metal without Torch host processing. Standard-library
adapters coordinate still/GVID/timelapse/ProRes containers; the existing Python
Torch adapters remain optional alternatives. [Stock UPRESABLE](stock-upres-workflow.md)
now provides timelapse/regression/both command orchestration, exact final-GPR
sequence packing and optional DNG/deep ProRes exports. Mac and Pi synthetic
functional tests pass. Two real 12 MP captured-GPR Pi inputs also produce
independently verified 8192x6144 outputs, averaging 57.12 s per frame offline;
broader scene/motion quality remains open. The standalone native app and all
three native residual engines are implemented; native simulation/regression
analysis continues through the existing analysis workflow.
The optional [stock FFmpeg decoder](stock-ffmpeg-decoder.md) now replaces the
old fused adapter with explicit stock GVS2/GVS3 packet decoding.

### Extended Mission 1 host replay

A later matched run uses the existing relocated A/V package for **1200
attempts per run (60 seconds each)** at 20 fps, still alternating the two
camera-still-derived RAWs. Recording-only and combined recording/preview each
write 1200/1200 frames without overruns or late writes; all **2400 payloads**
match independent stock bytes. The combined run delivers all 1200 full-frame
1024x768 previews with no drops, replacements or late deliveries. Durable
throughput is **20.0098 fps**, capture-to-preview p95 **30.8781 ms**. Encoder
workspace peaks at 97,936,896 bytes with zero fallback allocation in both runs;
combined process peak RSS is 585,891,840 bytes. These are measured replay
figures, not a camera memory budget or a sustained thermal qualification.

[Full receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-minute-mac.json) and
[package/source validation](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-minute-validation.json)
bind the results to installed binaries. This extends the six-second evidence
without replacing the actual camera, Pi, display, SD or temporal gates. The
different encode medians between these single runs are not evidence that
enabling preview accelerates encoding.

Read-only [PR status checks](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/preserved-prs.json) confirm
fork #95/#96/#100 and upstream #71/#72 remain open on their original bases.
No remote PR mutation, source upload or publication was performed.

The delivery priority above narrows the immediate video integration path; it
does not turn unresolved quality or hardware requirements into completed work.
Historical platform receipts remain bound to their recorded revisions.

| Task | Current progress and authoritative evidence | Still open |
|---|---|---|
| Optimized stock encoder | Streaming/parallel/NEON foundation implemented; [Pi latency evidence](stock-encoder-sub100.md), [fresh exact still payload/pixel comparisons](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/stills-mac.json), [240 exact replay payloads](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-mac.json) | Universal latency guarantees; actual camera execution |
| Broad RAW still formats | Four CFA phases, 12/14/16-bit words and legacy packed-12 supported; [format/conformance scope](stock-still-formats.md) | Broader real-camera fixtures and metadata cases; stock log precision remains lossy |
| Still presets/workflow | Six explicit presets, validated source calibration and editable DNG/GPR; [36 fresh pairs on two lossless sources](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/stills-mac.json) | Restored-tier/model quality selection; no inherited old-tier approval |
| Calibration/editable RAW | Typed supported calibration, EXIF/GPS/opcodes and per-frame precedence implemented; [contracts](stock-editable-raw.md), [independent core export pixels/calibration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/exports-mac.json) | Unsupported metadata/lens operations and broader reader interoperability |
| Bounded capture API | Ownership, leases, backpressure, cancellation, flush and writer failures implemented; [capture contract](stock-capture-api.md) | Physical sensor/DMA/storage adapter integration |
| Checked recording/recovery | Actual timestamps, CRC metadata/payloads, END3 and explicit prefix recovery implemented; [recording contract](stock-capture-api.md) | Live sensor/card interruption and durability evidence |
| Native calibrated preview | Immutable plans, source color/GainMaps/orientation and bounded queue implemented; [preview contract](stock-calibrated-preview.md), [combined path](stock-capture-preview.md). Independent color checks now cover all twelve CFA/depth combinations and 576 configurations on Mac/Pi; [retained validation and rejected register experiment](stock-preview-register-experiment.md) | Rear-display backend and broader lens/look/temporal approval |
| Mission 1 20-fps path | Historical Mac replay verifies 1200 exact recordings/previews with zero late deliveries. Current [bounded-spacing Pi source](stock-preview-colors.md) verifies all 600 recordings/previews per run against independent references; final/old comparison and cache counters are archived | Strict zero-late gate remains unmet: final runs have 109/311 late writes and 322/451 late previews. Pacing, final fsync and source/clock variability remain in the measurements; no universal throughput or physical-camera claim |
| Offline 1x reconstruction | Portable inference, editable/sequence export and [explicit still GPR re-encoding](stock-cnn-quality.md#optional-reconstructed-gpr) work; the later [half-strength checkpoint](stock-cleanup-strength.md) passes the declared numeric guard and is supplied. Earlier full-strength failures remain recorded | Broader model/tier/temporal approval; re-encoding can erase gains; old quality claims do not transfer |
| Offline 2x/8K reconstruction | Explicit architecture/geometry/calibration paths and fresh training/evaluation implemented; [functional evidence](stock-cnn-portable.md) and the supplied [source-wide diagnostic](stock-sr-source-regions.md) pass declared numeric/geometry/Pi checks | Small pooled gain (+0.00729 dB); useful recovered-detail, restored-tier and perceptual/temporal approval remain open; [native checkpoint qualification](stock-sr-native.md) passes scoped count/RAW guards |
| Native Core ML inference | Optional 1x/2x residual execution, float32 default/experimental float16, checkpoint/package identity checks, four unit settings and still/GVID/ProRes integration implemented; [contract and receipts](stock-coreml.md). A standalone no-Torch option is available through the native runtime | Per-operation Core ML device-placement proof; model/temporal approval; Python and standalone host arithmetic can differ at floating-point boundaries |
| Direct MPSGraph/Metal hybrid | Original residual engines ported with checked fp16 exports, bounded context reuse and current host baseline/scale math. Both scales/engines and media workflows pass; real 12 MP to 8192x6144 hybrid upres reaches stock GPR/GVID/MOV and calibrated ProRes. [Contract and receipts](stock-metal.md) | Native fp16 is not bit-identical to Torch (real maximum 8/7 counts); cold-start/sustained timing, model and temporal approval remain open |
| Standalone native host/runtime | Objective-C++ raw executable, scalar/NEON float32 host kernels, explicit 1x/2x artifacts, and standard-library still/GVID/captured-timelapse/direct-ProRes paths run without Torch/NumPy/Core ML Tools. [Contracts and receipts](stock-native-runtime.md) | Historical monolithic CLI syntax is not retained. Native simulation/combined/regression analysis still requires the analysis workflow. Numerical agreement does not approve models or motion |
| UPRESABLE orchestration | Timelapse/regression/both, explicit presets/checkpoint, half-capture simulation or original captured-GPR input, full stock GPR, exact GVID/MOV and optional DNG/ProRes implemented. Two real 12 MP Pi captures produce independently verified 8192x6144 outputs at 57.12 s/frame offline; [contract](stock-upres-workflow.md), [Mac receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-upres-receipts/mac.json), [Pi receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-upres-receipts/pi5.json), [real Pi receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-upres-receipts/pi5-real-captured.json). Native accelerated captured-GPR timelapse also passes on Mac | Broader scene/motion quality; native simulation/regression analysis uses the existing analysis workflow; no model approval or real-time CNN claim |
| Exact MOV interchange | Complete original stock GVID survives wrap/unwrap; [transport evidence including 4.42 GB/64-bit chunk offsets](stock-mov.md), [core full-recording identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/exports-mac.json) | Latest auxiliary-profile platform/version exchange |
| PCM/timecode interchange | Optional declared PCM16/24/float32 mono/stereo, independent timecode clocks, exact WAV samples and ProRes inheritance implemented; [120-frame A/V receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/real-core.json), [24 Release cases](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/release-tests.txt) | Actual camera audio/synchronization and broader editor compatibility |
| Calibrated ProRes | Deep HQ/4444, original timing and optional reconstruction implemented; [core decode](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/exports-mac.json), [one-TIFF scratch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prores-receipts/streaming.json), [A/V pixel/packet identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-av-receipts/real-core.json) | Broader rendered-color/lens/temporal approval and custom grading looks |
| SDK ABI and selected boundaries | Published C symbols, owned-copy rollback and encoder/decoder boundary fixes implemented; [C API](stock-sdk-api.md), [encoder](stock-encoder-bounds.md), [decoder](stock-decoder-bounds.md) | Broader malformed-input/failure/concurrency audit, deferred from immediate delivery |
| Independent packages | Four current MOV-enabled profiles pass relocated C/C++/SDK and installed PCM/timecode wrap/recovery; [current packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-restored-gpr-receipts/packages.json), [installed 1x/2x reconstructed GPR/DNG and Mission 1 binary identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-restored-gpr-receipts/installed-validation.json). Fork pushes are authorized and draft #101 contains the native runtime | Latest hosted platform/release validation; packages and receipts remain revision-bound |
| Actual camera production | Integration requirements documented; host/Pi evidence stays explicitly labeled | Sensor/DMA, display presentation, durable SD, thermal and physical A/V synchronization receipts |
| Optional FFmpeg decoder | Stock GVS2/GVS3 adapter for pinned FFmpeg n8.0; exact twelve-format Bayer matrix, frozen fixtures, corruption rejection and PNG/ProRes pass on Mac and Pi; hosted x86 gate passes at `eaeaefa`; [contract/receipts](stock-ffmpeg-decoder.md) | Patched build required; older MOVs without configuration need unwrap/rewrap; no calibrated color claim |

## Feature accounting at the start

| Previous high-level capability | New foundation | Rebuild work |
|---|---|---|
| Editable RAW stills and camera metadata | Stock GPR/DNG SDK and encoder | Reconcile metadata preservation, still quality selection and restoration workflows |
| High-bit-depth and four-phase Bayer workflows | Stock formats and an optimized RGGB14 path | Audit and port 14/16-bit and CFA support with explicit format/metadata tests; keep historical format paths byte-compatible |
| Compact still tiers | Stock presets 0–5; historical tiers used other profiles/models | Re-establish size/fidelity tiers; do not relabel old Q8 or old quality results as new measurements |
| Independently decodable RAW video | Native replay writer with stock VC-5 payloads | Turn the replay into a usable library-backed recording workflow |
| Bounded capture lifecycle | Synchronous serial replay and reusable allocator | Recreate create/submit/flush/destroy, ownership, cancellation, writer-error propagation and bounded backpressure |
| GVID framing and metadata | Experimental v2/VC5 profile, nominal PTS, strict inspector | Preserve actual capture timestamps, camera/CFA/black/white/color metadata and interrupted-recording behavior with explicit versioning |
| Native full-frame camera preview | Stock decoder available; preview not integrated into recorder | Decode the same recorded stream to a bounded full-frame RGB preview and measure its cost |
| Offline 1x Bayer cleanup | Explicit model adapter, tested on two frames | Reconcile model identity/codec pairing and quality; existing tested weights worsen replay-reference PSNR |
| Offline 2x/8K Bayer SR | Old fused-profile model does not automatically transfer | Rebuild inference and editable packaging, validate an appropriate model against stock input; separate functional support from quality approval |
| MOV without re-encoding | Not yet ported | Preserve stock compressed payloads through wrap/unwrap with explicit codec/profile identification |
| RAW-derived ProRes and editable DNG/GPR | Stock SDK available; old renderer expects fused GVID | Rebuild stock-payload dispatch and metadata-preserving export; retain optional reconstruction |
| Build profiles, distribution and documentation | Opt-in host experiment | Recreate independent build options and cross-platform delivery around actual supported features |
| Quality, conformance, CI and hardware receipts | Stock-byte tests, sanitizer tests, 3,360-frame Pi replay evidence | Adapt relevant old tests and add integration/ownership/failure/metadata/preview/export/model-contract checks |

## Work order and completion evidence

1. **Capture and storage architecture.** Extract reusable framing and recording
   interfaces. Add bounded ownership and asynchronous storage with explicit
   backpressure/error policy. Test cancellation, failed writers, flush and
   complete-frame preservation. Measure real absolute deadlines rather than
   process CPU time or average buffered FPS alone.
2. **Metadata, stills and preview.** Recreate camera metadata and supported
   Bayer workflows, implement preview from the same stock stream, verify
   dimensions/color and correct buffer lifetimes. Audit high-bit-depth paths
   before making parity claims. Keep the fast RGGB14 path and compatible
   fallbacks separately testable.
3. **Interchange and reconstruction.** Port MOV/RAW/ProRes workflows to the
   explicit stock payload kind. Restore 1x and 2x model contracts and editable
   outputs without assuming the old fused-profile checkpoints are approved
   for the new input. Evaluate held-out detail/color/noise and temporal
   behavior as well as numeric RAW distortion.
4. **Integrated verification and replacement PR.** Run meaningful native and
   SDK checks, relevant platform builds, new Pi capture/preview/end-to-end
   measurements, and applicable quality tests. Publish one separate reviewable
   fork draft with an implementation/evidence matrix and explicit remaining
   hardware/model limitations. Keep the old PRs open.

## Baseline constraints to preserve in reporting

### First implementation milestone (in progress)

The [stock capture API](stock-capture-api.md) now implements bounded
copy-before-return and explicit input leases, asynchronous writing,
flush/finish/cancel/error handling, actual timestamps, opaque clip/frame
metadata, checksum-protected v3 framing and explicit interrupted-prefix
recovery. Native C/C++ and independent Python tests exercise these contracts.
The codec entry point also serializes global codebook state across captures
and still SDK calls, and skips an unrequested thumbnail allocation.

The [native preview](stock-preview.md) now renders full-frame reductions from
the same stock recording, with independent stock RGB fixtures and Pi playback
measurements. The [bounded asynchronous capture preview](stock-capture-preview.md)
now connects recording to a calibrated RGB callback with copied payloads,
retained source plans, latest-pending replacement and isolated sink failure.
Matched 900-attempt Pi runs show 256x192 preview is much cheaper than 512x384;
capture overruns and late completions remain.
The [prepared calibrated preview](stock-calibrated-preview.md) additionally uses
source matrix/white-point/black/white/GainMap data and crop/orientation, with an
SDK-independent immutable plan and four-pixel NEON processing. It is a reduced
wavelet approximation; full look/lens/temporal approval, physical display
integration and reliable capture/display scheduling remain pending.

The [metadata and editable RAW workflow](stock-editable-raw.md) now carries
per-source calibration through recording and exports decoded DNGs or GPRs
without re-encoding their stock payloads. Independent TIFF reads and SDK
decodes verify calibration, EXIF/GPS, compressed bytes and Bayer values.
Standalone uncompressed DNG packaging supports four CFA phases and 12/14/16-bit
counts. The video frontend now maps these domains to the stock encoder.

The [independent build profiles](stock-build-profiles.md) now separate the
capture, preview and desktop SDK dependencies, export installed CMake targets
and generate relocatable static-library ZIPs. Package checks remove the SDK
and unused codec sources from minimal builds, delete producer paths and run
outside C/C++ clients against the extracted archive. Full packages also check
the installed Python/native editable-export workflow.

The [low-level Bayer format port](stock-bayer-formats.md) now accepts all four
Bayer phases with word-packed 12/14/16-bit counts while retaining the stock
12-bit logarithmic transform. It does not import the earlier codec's higher
internal precision. The [still SDK port](stock-still-formats.md) adds its public
format mapping, spatial black-level handling, source matrices/illuminants and
supported GainMaps. The [still preset workflow](stock-still-quality.md) adds
explicit Q0–Q5 selection and exact typed-calibration packaging around the SDK,
with lossless-DNG and Pi replay measurements. Restored still-tier/model quality
approval and broader metadata support remain pending. Video capture, extraction,
editable RAW packaging and calibrated preview now cover all 12 Bayer/count
combinations; the optional CNN adapter remains RGGB14-only.
The [decoder boundary audit](stock-decoder-bounds.md) now checks public arguments
and every allocator-failure point reached by an independent stock RAW/RGB
fixture. Arbitrary malformed entropy streams and wider SDK error handling
remain separate work.

The [public SDK C API audit](stock-sdk-api.md) repairs two missing published
symbols, adds an allocator-aware parameter copy with rollback, makes owned
parameter cleanup repeatable and classifies VC-5 metadata without image decode.
Real C callers, allocation-failure tests and installed clients exercise the
contracts; broader geometry/buffer and still concurrency auditing remain open.

The [encoder boundary audit](stock-encoder-bounds.md) additionally rejects tiny
or overflowing geometry and short row capacity before codec allocation, and
normalizes unaligned/odd-stride historical Bayer layouts through scratch
allocated with the caller's callbacks. Valid aligned callers retain their existing fast path and
stock-byte evidence. Full compressed-decoder and SDK failure/concurrency audits
remain separate requirements.

The [optional still reconstruction workflow](stock-cnn-quality.md) now produces
calibrated editable DNG using the existing stock-Q3 1x model. Twenty-four
lossless-reference comparisons show modest aggregate improvements, including
a Q5 scene regression. This is functional/model-evaluation evidence, not
restored-tier or 2x quality approval; training-split membership is unknown.

[Portable reconstruction](stock-cnn-portable.md) now bundles the legacy BIBO
architecture and supports explicit 1x/2x still and recorded-sequence inference,
including scaled crop/EXIF and phase-correct GainMap metadata. Four legacy 2x
checkpoints all regress against bicubic in sixteen simulated-stock comparisons;
no 2x model is approved. A [fresh stock-matched candidate](stock-sr-training.md)
now has source-grouped training and frozen final evaluation. Its modest overall
gains and shadow regression leave the full quality gate unfinished.

[Stock MOV interchange](stock-mov.md) now preserves complete stock GVID files
through private GVS2/GVS3 MOV sample profiles, including original payloads,
metadata and timestamps. Independent packet checks, Pi SDK pixels/calibration,
Mac/Pi FFmpeg-version exchange, sanitizer checks and relocated packages validate
the transport. It is not standard CFHD or rendered ProRes. Optional declared
PCM audio and nominal-rate timecode now preserve video bytes/timestamps, with
sample-exact WAV extraction and independent timecode clocks.

[Calibrated ProRes rendering](stock-prores.md) now processes each actual typed
DNG through the macOS RAW decoder into a 16-bit Rec.709 intermediate and deep
ProRes HQ/4444, with explicit frame durations and independent packet/color checks.
Acknowledged frame delivery now reuses one deep TIFF instead of retaining
the entire rendered sequence; the original batch manifest interface remains.
Both caller-selected 1x and 2x CNN paths run through this renderer. Real camera
functional checks include 8192x6144 12-bit 4444 output; broader rendered/temporal
quality approval remain pending. Declared stock-MOV audio/timecode now carry
through ProRes export; caller-supplied synchronized WAV/labels are also accepted.

This is not feature-suite parity. Restored still tiers, broader rendering validation, full reconstruction/model quality
selection, broader preview color/lens validation, integrated capture/display performance
and remaining platform/release integration remain
pending. Earlier
feature/validation PRs remain open and unchanged.

The #100 experiment used preloaded RAWs decoded from two supplied GPR stills,
not a live sensor sequence. On Pi 5 the 900-frame Q3/Q4 runs averaged about
30 fps before final flush, but missed 231/900 and 294/900 completion deadlines;
final flush reduced total throughput to 28.47/28.22 fps. Q3 per-frame fsync
measured 23.44 fps. Storage was USB SSD, not microSD. Four channel workers
used the four CPU cores; preview and CNN were outside capture.

Existing stock-Q3 BIBO1x inference works, but reduced PSNR relative to those
two replay references. Those references are already compressed-camera output,
not sensor truth. Do not enable restoration by default or inherit the old
fused-codec model's visual approval without new evidence.

Actual sensor delivery, firmware integration, on-camera display presentation,
durable microSD recording and camera-specific thermal behavior remain
hardware integration work. Host evidence must identify those boundaries.

The optional [compute-stage policy](stock-preview-compute-stages.md) serializes
preview against the next encode while allowing writer/preview overlap. Matched
600-attempt ABBA replay improves durable fps from 16.41–16.76 to 18.28–18.63,
with exact accepted payloads/pixels. All runs still overrun and miss deadlines.
The policy remains opt-in; 24 Mac/21 Pi Release tests and four relocated
package profiles pass for its source-bound implementation.

The subsequent [direct-output reconstruction](stock-preview-direct-output.md)
removes a profiled full-plane copy and unused final highpass bands while
preserving intermediate alignment and exact pixels. Four serialized runs retain
all 600 attempted frames/previews each, with durable fps 19.9417–19.9988 and
used scratch approximately 11.10 MB versus 30.57 MB. Hundreds of late deliveries
still prevent strict floor approval. The independent retained-band matrix now
also covers all twelve CFA/depth formats, six presets and odd packed row pitches.

Two further [kernel experiments](stock-preview-rejected-kernels.md) are rejected
after measured comparison: an exact bounded-token NEON dequantizer gives no
consistent workflow improvement, and per-row output addressing is slower in
isolation. Neither remains in production. Runtime source is restored to
`f1e08c8`, the Pi executable matches its prior SHA-256 byte for byte, and fresh
26-target Mac/23-target Pi Release runs pass. A hosted Mac allocation failure
is recorded separately from code validation and retried only after it became
terminal; six other hosted jobs passed for that implementation.

The [residual-strength study](stock-cleanup-strength.md) now supplies an explicit
1x checkpoint passing the unchanged per-group validation guard and every
active/shadow/highlight/detail region on four full-resolution references.
Pooled active PSNR improves 0.639 dB; Pi CPU execution and Mac MPS agree within
one count on two supplied 12 MP sources. Pi inference remains offline at about
96 s/frame. Photos/training arrays remain external; the optional selected
checkpoint is supplied in the repository. Broader/2x quality remains open.
A [prepared-inverse architecture](stock-prepared-inverse.md) was also tested
and discarded for slower encode/reconstruction medians; runtime source and
the prior Pi executable are restored exactly, with 26 Mac/23 Pi tests passing.

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

The new [full-image model selector](stock-sr-full-selection.md) writes an explicit
candidate/data/code protocol before validation and requires per-image regional
nonregression plus pooled active improvement. SDK-backed tests pass on Mac/Pi;
all ten real patch-qualified candidates fail, so final testing is skipped.
The camera codec/runtime and selected 1x weights are unchanged.

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

The retained [preview-plane placement](stock-preview-colors.md) uses spare
reserved bytes for distinct low cache-set offsets, keeping dense fallback and
packed-row/int16 alignment contracts. Final source passes 27 Mac/24 Pi Release
targets and four relocated profiles. Exact-frame and counter improvements do
not close strict Pi deadlines or broader CNN/restored-tier quality.

The [native 12 MP GoPro timing follow-up](stock-native12-gopro-timing.md) uses
the unchanged production binary and FIFO1 profile on GP011728/GPAB1094 counts
without downsampling. Both fixed 600-attempt runs fail the combined floor:
600/570 accepted, 32/570 late writes and 223/570 late previews. All accepted
payloads, calibration and preview pixels remain exact. Earlier scoped FIFO
passes do not establish timing for these inputs. All seven hosted jobs pass
at source revision `3a52439`; this follow-up changes evidence, not runtime.

A subsequent [explicit horizontal NEON comparison](stock-horizontal-inverse.md)
is discarded after its 9.05% isolated Pi kernel gain fails to establish a
consistent native-input combined recording/preview win. All 2,318 accepted
comparison payloads and previews are exact; all four strict floors fail.
Production source/binary remain unchanged. Hosted revision `9111564` now passes
all seven jobs, with the full receipt archived in that follow-up.

The [retained-band cost study](stock-preview-retention.md) identifies 6.7–7.0 ms
before inverse reconstruction in isolated native-input comparisons. A band-gap
proposal and two scratch-row variants are discarded after broader controls;
all 4,605 accepted combined frames/previews remain exact, but all eight timing
floors fail. Runtime source/executable are restored, with fresh 27 Mac/24 Pi
Release passes. Baseline 779bb47 passes all seven hosted jobs.

The [historical-width/pretraining study](stock-sr-historical-recipe.md) adds three controlled comparisons on stock pairs: 2,400 updates and 40 complete-image candidate inferences. Old pretrained initialization is substantially better than fresh initialization under the matched budget. Shadow-constrained models improve full-image pooled fidelity by up to +0.57218 dB but fail regional guards; no replacement is selected. Border artifacts and interior overshoot both need further work.

The [boundary/input-gate continuation](stock-sr-historical-boundary.md) adds 104 complete-image candidate outputs, a frozen diagnostic and CPU/MPS historical-reference checks. All RAW regional guards pass, including GP011689 at +1.13707 dB, but rendered RGB is less improved than the supplied model. Its diagnostic evidence and local 1:1 review remain separate from production/native/temporal quality approval.

The [calibrated mixed-camera continuation](stock-sr-calibrated-mixed.md) recovers archived per-frame camera metadata and audits 1,088 actual-stock patch pairs. A concrete CLI parameter-import failure on 65x65 gain maps is corrected and verified on Mac/Pi. Four training cases complete 3,200 updates; all eight checkpoints fail Nikon shadow guards, so model/backend quality status remains unchanged.

The [sensor-gate and frozen-cascade follow-ups](stock-cnn-followups.md) complete the bounded comparisons without a replacement: 18 patch settings, 20 full gate outputs, ten complete cascade outputs and actual rendered comparisons. The only gate survivor is worse than supplied BIBO in pooled RGB; the cascade improves rendered Nikon fidelity but regresses every camera validation image. No thresholds or failed guards are relaxed. A separate [JSON boundary correction](stock-sdk-parameter-json.md) fixes the specific new hosted parameter-import failure, with fresh focused Mac/Pi passes.

The optional [streaming encoder-derived preview](stock-wavelet-stream-prototype.md)
now consumes bounded wavelet rows through public encoder/capture/preview APIs.
Both new native-input Pi runs record and preview all 600 frames with exact
payloads and calibrated pixels. They reach 19.9965/19.9980 durable fps, with
171/47 late previews, so the strict 20 fps deadline gate remains open. Fresh
Mac/Pi contracts and four relocated Mac package profiles pass; hosted results
must be evaluated at the delivered revision.
