# Stock-compatible RAW media: what we are building

## Goal

Recreate the useful high-level workflows from fork PR #95 and its validation
PR #96 on the stock-compatible VC-5 / 12 MP foundation in #99/#100. Deliver one
reviewable fork PR with working still, recording, preview and desktop paths.
Keep the older PRs open and preserve the evidence behind both architectures.

The product idea is simple: capture editable Bayer data and calibration now;
choose rendering and optional reconstruction later. The stock encoder replaces
the earlier fused codec. That replacement needs its own compatibility,
performance and model-quality evidence.

## The three user workflows

### 1. Editable RAW stills

Source DNG/GPR or declared Bayer RAW becomes a compressed GPR still and/or an
editable DNG. The caller chooses a stock compression preset and declares the
sample domain. Supported calibration stays attached to the image.

Implemented support includes RGGB/GBRG/GRBG/BGGR, 12/14/16-bit counts, packed-12
SDK inputs, six stock presets and a verified real 102 MP X2D case. Support for
one camera or the declared metadata contract is not universal editor/lens
compatibility. [Formats](stock-still-formats.md),
[quality/presets](stock-still-quality.md), [metadata](stock-editable-raw.md).

### 2. RAW video and native preview

Bayer frames become independently decodable stock VC-5 records in GVID, with
actual timestamps and per-frame source calibration. Bounded capture accepts
copied or leased inputs, handles backpressure/cancellation/errors, and writes
asynchronously. Native calibrated preview works without a camera-side CNN.

Implemented software includes CRC/end-marker checks, explicit incomplete-prefix
recovery, an encoder-derived streaming preview and a bounded display queue.
The API is the integration boundary; the tested replay/memory sink does not
supply a sensor or physical display adapter. [Capture contract](stock-capture-api.md),
[preview](stock-capture-preview.md), [streaming preview](stock-wavelet-stream-prototype.md).

### 3. Desktop extraction, finishing and optional reconstruction

A recording can recover editable Bayer frames or wrap its original compressed
records in MOV without image re-encoding. The desktop path supports declared
PCM/timecode and calibrated deep ProRes. Optional 1x cleanup and 2x reconstruction
can produce editable RAW and explicitly requested stock re-encodes.

Portable and native Apple backends implement the declared model/runtime
contracts. CNNs are optional, offline, and caller-selected. The current
supplied-model recording-to-editable-RAW-to-ProRes proof is a two-frame
integration check, not motion-quality approval. [MOV](stock-mov.md),
[ProRes](stock-prores.md), [reconstruction workflow](stock-upres-workflow.md),
[current model/media chain](stock-selected-video-chain.md).

## Where we are

| Area | Delivered | Still open |
|---|---|---|
| Stock encoder and stills | Declared formats/presets, supported metadata, historical payload/pixel checks | Broad camera/editor coverage; inherited stock conversion is lossy |
| Capture and framing | Bounded ownership/lifecycle, timestamps, calibration, CRC/recovery | Physical sensor/storage integration |
| Native preview | Calibrated software preview and streaming encoder rows | Reliable native 12 MP combined zero-late timing; physical display |
| Desktop outputs | Editable frames, exact MOV recovery, declared A/V, ProRes | Broader interoperability and motion/look approval |
| CNN execution | Portable/native offline 1x/2x paths with editable output | Useful broad 2x detail, temporal quality and approved restored tiers |
| Packaging/validation | Four package profiles, public C/C++ consumers, Mac/Pi checks and hosted CI | Final acceptance audit and readable commit history |

The supplied 1x checkpoint passes its scoped numeric and supplied-camera checks.
The supplied 2x checkpoint has small measured gains. Later candidates that fail
regional fidelity checks stay research artifacts; successful inference and
lower synthetic shift error do not qualify them for promotion.

Current native 12 MP comparisons record and preview all 600 attempts in the
adopted candidate runs, but retain late deliveries and durable rates below
20 fps. This remains a failed acceptance gate, not a nearly-passing approval.
Exact revisions, measurements and limitations live in the
[requirement ledger](stock-rebuild-requirements.md).

## Finish line and next steps

1. **Make the existing work reviewable.** Put workflows and boundaries first;
   arrange the changes into the eight feature commits in the
   [review plan](stock-review-plan.md). Keep tests with their feature and
   detailed experiments behind evidence links.
2. **Close the native Pi acceptance gap.** Retain the same correctness,
   every-frame preview and real final-durability requirements while resolving
   missed deadlines. Do not substitute encoding-only timing for the combined
   workflow.
3. **Qualify reconstruction quality.** Require applicable per-camera RAW and
   rendered checks, then actual motion/temporal evidence before approving new
   2x/restored tiers. Keep current model limitations explicit meanwhile.
4. **Audit the final PR.** Check original feature/validation coverage, package
   consumers, current CI and reproducible evidence against the final revision.
   Report unsupported/deferred capabilities explicitly.

This reviewability pass changes documentation only. Broad ASan/UBSan/fuzz
expansion remains deferred at the user's request. Sensor/DMA, rear-display, SD and thermal
camera validation remain unverified; they are not silently counted as delivered.

## Review without reading the experiment log

Start with this page, then [the proposed commit order](stock-review-plan.md).
Use the [original feature/validation map](stock-feature-parity-audit.md) to check
parity and the [requirement ledger](stock-rebuild-requirements.md) for acceptance.
Build commands remain in the [core workflow](stock-raw-media-rebuild.md#core-workflow).
Detailed research and measurement history remain available through the
[evidence guide](stock-evidence/README.md); they are supporting material, not
the PR's opening narrative.
