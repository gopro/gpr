# Streaming encoder-derived preview

The optional `gpr_capture_app --preview-streamed` path reconstructs quarter-scale
GS/RG/BG planes while encoding, then uses the existing calibrated preview queue.
It replaces complete upper-band retention with bounded row storage. Recorded
stock VC-5 payloads and calibrated preview pixels remain exact in the comparisons
below. The strict sustained native 12 MP / 20 fps deadline gate remains open.

## API and ownership

`vc5_encoder_process_with_preview_rows` shares the ordinary encoder's validation,
packed-input normalization and global codec lock. Callbacks borrow quantized
rows until return. Channels can call concurrently; each channel preserves its
row order. Do not reenter the encoder or throw across its C boundary. There are
no callbacks after the encoder returns. Existing encoder and complete-band
observer APIs retain their contracts and public structure layouts.

`gpr_preview_stream_create/push/finish/reset/destroy` provide the reusable C
consumer in `gpr_preview`. Creation bounds geometry and sample-buffer capacity;
there is no per-row allocation. Invalid rows latch a frame error. Finish, called
after producer callbacks return, exposes borrowed immutable final planes or
clears its output on failure. Copy/render those planes before reset/destroy.
Neither operation may overlap a push or a borrowed-plane reader.

The inverse dequantizes each highpass row once. Per channel it retains three
upper rows, four reconstructed LL rows, and eight lower highpass rows for even
lower-band heights. Odd heights use sixteen: bottom-edge flushing can require
nine live rows. Complete final planes remain allocated. Native 4096x3072 uses
4,843,522 bytes of sample buffers, including final planes and the cubic table;
this excludes vector/object bookkeeping and allocator overhead.

`gpr_capture_create_with_encoder_rows` carries frame index and PTS through the
callbacks. Its existing encoded-frame observer runs after encoder callbacks
join and the input lease is released. The frontend finishes the inverse,
submits copied planes to the calibrated queue, and resets the stream before the
next encode. Preview errors latch independently while recording continues.
Cancellation and sink failures retain the existing capture lifecycle. The
capture-only library still links without the decoder or preview implementation.

## Reproduction

Build with video, preview and codec tests enabled, then run CTest. The public
contracts are `wavelet_stream_public`, `wavelet_stream_public_fast` and
`capture_stream_lifecycle`; the independent coefficient and inverse-row
comparisons are `wavelet_pipeline_coefficients` and `preview_inverse_rows`.

For replay with already prepared quarter-scale profiles:

```sh
build-runtime/gpr_capture_app --width 4096 --height 3072 --cfa RGGB \
  --sample-bits 14 --quality 3 --fps-num 20 --fps-den 1 --queue 4 \
  --workspace-mib 192 --leased --writeback-mib 1 \
  --input frame.raw --source-metadata calibration.json \
  --preview-profile profiles/000000.gprp --preview-streamed \
  --preview-every 1 --preview-sync-stages --preview-foreground \
  --frames 600 --paced --output new-streamed.gvid
```

Repeat input, metadata and profile arguments in matching order for alternating
sources. `--preview-streamed` requires quarter-scale profiles. It is optional;
`--preview-from-encoder` retains the complete-band route. The archived driver
records the exact two-source commands and the previously used child-only FIFO1
policy. No global scheduler, device or boot setting changes accompany this work.

## Validation and full-workflow Pi results

Fresh Mac full-SDK Release: **29/29 CTests**. Fresh Pi SDK-less Release:
**23/23 CTests**, plus **8/8 frontend tests** with the unchanged reference profile
compiler. Mac frontend tests pass with a fresh full-SDK compiler. All four Mac
capture/preview/camera/full package profiles pass relocated public C/C++ clients;
the camera package is built after physically removing SDK sources.

The public streaming matrix compares ordinary encoder payloads and independently
entropy-decoded preview planes across 497 format/quality/geometry cases, each
with two reused frames, plus unaligned packed-12 inputs. Additional contracts
cover invalid rows/capacity, reset, leases overwritten on release, cancellation,
writer failure, isolated preview error and absence of callbacks after finish.

The fixed Pi ABBA comparison alternates two native 4096x3072 GoPro sources for
600 attempts per run at requested 20 fps. It includes calibrated 1024x768 memory
CRC preview on every frame, serialized compute, 1 MiB writeback and final fsync.
No other test, build or profiler runs during the timing children.

| Order / mode | Recorded / attempted | Durable fps | Encode median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| 0 retained control | 548/600 | 18.1598 | 40.4288 | 548 | 548 |
| 1 streamed | 600/600 | 19.9965 | 37.7180 | 154 | 171 |
| 2 streamed | 600/600 | 19.9980 | 35.3749 | 44 | 47 |
| 3 retained control | 599/600 | 19.9643 | 36.3029 | 76 | 76 |

Both streaming runs record and preview every attempted frame, with zero
overruns or discarded previews. **All four runs fail the strict gate**: zero
late writes/previews and durable fps at least 20. Controls vary substantially;
this comparison does not establish a universal percentage speedup.

Encoder workspace falls from 104,257,536 to 97,936,896 bytes. Peak process RSS
is about 570.5 MB streamed versus 597.8 MB retained in these runs. These are
separate measurements from the stream's sample-buffer accounting.

The existing independent verifier checks all **2,347 accepted payloads**, source
calibration, attempt/PTS binding, delivered preview pixels/CRCs and END3 before
each temporary clip is deleted. Persistent clip hashes and numeric logs remain.
A second audit independently checks source hashes, event accounting and deadline
verdicts; it does not claim to recheck deleted clip bytes. [Audit](stock-wavelet-stream-receipts/audit.json)
and [archived protocols, code and numeric logs](stock-wavelet-stream-receipts/manifest.json)
retain both the gains and failures. No sensor, display, SD-card, temporal-CNN or
thermal deployment claim follows from warm-cache replay.

## Earlier isolated prototype

The preceding ordinary-user Pi kernel study has no rendering, queue, pacing,
writing or fsync. Final retained encode-plus-inverse medians are
43.39984/42.010214 ms; streaming medians are 39.977447/39.7899835 ms. Every final
streaming block median beats every retained block median. An intermediate
version is mixed/slower on the first source and remains archived, as does the
initial eight-row odd-height guard failure. Across three versions, 1,536 payloads
and 3,456 reconstructed planes compare exactly. These isolated results do not
replace the full-workflow measurements above.

## Inverse-row fixture correction

Hosted run [41184451458](https://github.com/dcliftreaves/gpr/actions/runs/41184451458)
found overflow in the original synthetic inverse-row fixture: arbitrary int16
highpass values were passed as entropy-decoded cubic symbols. That legacy
inverse input has magnitude at most 255; LL values are already expanded. The
fixture now samples that valid highpass range and explicitly includes -255,
255 and zero, retaining the large quantizers, full-range LL, odd dimensions,
border filters, both prescales and trailing guards. All 840 exact comparisons
pass locally, including a focused check with the affected legacy dequantizer
instrumented. Production code, supported payloads, assertions and the Pi timing
source are unchanged. The original fixture and its logs remain in the archive;
new hosted validation must pass before the revision is called green.

The corrected fixture and streaming source now pass all seven hosted jobs at
`3b2c0f0` ([run 41184771455](https://github.com/dcliftreaves/gpr/actions/runs/41184771455)).
[Post-streaming profiling and controls](stock-stream-followups.md) retain the
rejected color, unpack and entropy experiments separately from production.
