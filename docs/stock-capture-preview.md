# Capture with bounded asynchronous calibrated preview

`gpr_preview_queue.h` connects recording to a caller-supplied RGB sink. The
recorder offers an already-written stock VC-5 payload with its immutable source
plan, frame index and capture timestamp. The queue copies compressed bytes
before returning and retains the plan. RAW leases and writer-owned compressed
buffers may then expire normally.

There is one active frame and one latest pending frame. A newer offer replaces
pending preview work. It cannot interrupt rendering or a sink callback already
in progress. Offers do not wait for the renderer/sink; a short mutex covers
the bounded payload copy and bookkeeping. Preview still competes with capture
for CPU and memory bandwidth.

## Library ownership and failures

- Create reserves two compressed buffers and one RGB buffer within configured
  capacities (each 1..128 MiB). Decoder scratch and retained calibration plans
  have separate memory costs. The application controls its number of plans.
- An accepted offer owns its copy and plan reference. Indices/timestamps must
  strictly increase; gaps are allowed. Rejected offers acquire no ownership.
  `plan_retain` shares ownership; `plan_destroy` releases one reference.
- The sink runs on the preview thread outside queue locks. RGB is borrowed
  until callback return. The sink may query statistics but must not call offer,
  finish or destroy. Its context must live through finish/destroy.
- Callback failure/exception latches `GPR_PREVIEW_CALLBACK`; decoder errors
  propagate. Pending work is discarded; later offers return the latched error.
  Applications decide whether this also ends recording.
- Finish closes offers, drains the latest image and waits for callbacks.
  Concurrent finish callers are supported. Cancel closes/discards pending work;
  active work/callbacks may still finish. Destroy cancels, waits and joins, and
  must be serialized against all callers.
- Once drained, `offered == rendered + discarded`; replacements are a subset
  of discarded images. Inflight is at most two. Rendered means sink acceptance,
  without a claim that a physical screen presented the image.

`background=1` requests lower worker priority: Linux thread nice at least 10,
macOS utility QoS or Windows below-normal priority. It never raises an already
lower priority and does not change encoder priority. Statistics distinguish
requested/applied settings; unsupported/rejected requests report applied zero.
No realtime privileges are required.

## Capture frontend

`--preview-from-encoder` selects the quarter-scale retained-band path. Capture
reconstructs the three channels concurrently while the encoder coefficient view
is alive, then offers copied reduced planes to the same bounded display queue
when the completed payload is available. The encoded payload CRC binds display
to recording. This avoids entropy decoding. Existing compressed preview remains
the default and supports reductions 4/8/16; encoder preview requires reduction 4
and rejects other profiles before creating output. The new capture creator is
`gpr_capture_create_with_encoder_preview`; prior creators retain their ABI.

The `encoder_wavelets` summary identifies this path. Its `encode_ms` includes
coefficient reconstruction, while `render_ms` covers queued color/display only.
`payload_peak_bytes` now counts copied reduced-plane bytes for this mode.
Reconstruction now reuses first-touched per-channel scratch arenas; the queue still
reserves two bounded buffers and supports replacement, sink failure and drain.
Each arena is sized from quarter-scale sensor geometry (16 bytes per reduced
pixel plus 1 MiB headroom), capped at 64 MiB per channel before output creation.
Allocation aligns to 64 bytes and fails explicitly on exhaustion, with no heap
fallback. The callback owns the scratch until the queue has copied its planes;
the next frame then resets the arenas. Summary counters distinguish total
reserved scratch from peak used scratch. This does not remove thread creation
or other allocations elsewhere in capture.

The later [direct-output path](stock-preview-direct-output.md) eliminates the
final padded-wavelet-to-packed-plane copy and three unused final highpass
bands. Intermediate alignment/pitches remain unchanged, and odd packed output
rows are tested. At 12 MP, used reconstruction scratch falls to 11.10 MB while
the existing 40.89 MB reservation remains. Both matched serialized 600-frame
runs preserve every frame/preview, but still miss deadlines.

The [arena follow-up](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoder-arena-600.json)
reserves 40.89 MB and uses 30.57 MB peak over 600 frames; all payloads and preview
pixels remain exact with no drops. Preview p95 is 45.96 ms and encode/reconstruct
median 32.14 ms. Six previews and nine writes exceed 50 ms; the matched
recording-only baseline has three late writes. RSS peaks at 613.94 MB, versus
588.76 MB in the prior run. These single-run results establish bounded scratch
reuse, not a statistically proven improvement in tail latency or a passed
strict timing gate.

Two real Pi sources at 4096x3072 Q3/20 fps deliver all 120 exact previews with
46.06 ms capture-to-sink p95, zero late previews/writes and 20.02 buffered fps.
The 600-frame/30-second follow-up delivers all 600 exact previews without drops,
46.07 ms p95 and 20.01 buffered fps, but has four previews above 50 ms and five
late writes (the recording-only baseline also has five late writes). Final fsync
takes 1.047 s, reducing the combined durable average to 19.34 fps. Thus typical
latency meets the target while the strict sustained zero-late/durability gate
remains open. Receipts: [120 frames](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoder-preview.json)
and [600 frames](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoder-preview-600.json).

Build `gpr_capture_app` with both capture and preview. Supply one prepared pure
`.gprp` profile per RAW source in input order:

```sh
build-runtime/gpr_capture_app \
  --input a.raw --input b.raw \
  --source-metadata a.json --source-metadata b.json \
  --preview-profile a.gprp --preview-profile b.gprp \
  --preview-every 3 --quality 3 --frames 900 --paced --leased \
  --output new-recording.gvid
```

Use the [host preparer](stock-calibrated-preview.md) or equivalent source
parameters. Pure profiles are reusable for their RAW/calibration pair. Offline
`.gprp.bind` files bind an existing recording and are not loaded for a new
capture. The caller supplies matching calibration. Changing camera WB/matrices
currently requires another plan; these two-fixed-source tests do not establish
rapid dynamic-calibration preparation cost.

The frontend keeps the original source/profile association after overruns.
It now offers preview through `gpr_capture_create_with_observer`: the encoder
thread provides a borrowed completed payload before enqueueing that frame for
storage. The bounded preview queue copies it before the observer returns.
This removes the storage-write dependency; preview still waits for encoding.
The original `gpr_capture_create` and callback-structure ABI remain available.
An observed frame may subsequently be discarded on cancellation or writer
failure, so a preview event alone does not prove that the frame was recorded.
The lifecycle test blocks an encoded observer, cancels capture and verifies that
finish waits for observer release, its borrowed payload remains intact, no
frame is written/completed, and the observed frame is accounted as discarded.
Observers also exercise reentrant stats access outside the capture locks.
Its sink computes RGB CRCs in memory, with frame identity, geometry and render,
capture-to-sink and overlap measurements. There is no physical display backend.
Background priority is the default; `--preview-foreground` enables comparison.
`--preview-delay-ms`/`--preview-fail-after` simulate a slow/failing sink.

Sink failure stops preview offers while recording continues through END3 and
fsync. The frontend prints the complete capture summary, then exits nonzero to
report preview failure. Preview events/summary precede the existing final
capture summary. Durable capture FPS includes fsync; remaining preview drain
and setup/precomputation time are reported separately.

## Pi 5 measurements

Two alternating supplied 4096x3072 RGGB14 sources, their individual calibration,
stock Q3, leased inputs, four capture slots, 192 MiB workspace, nominal 30 fps
paced replay and USB SSD. Each sustained run attempts 900 frames over 30 seconds
with final fsync. This is one matched run sequence, not a statistical overhead
estimate. Preview requests occur every third accepted frame.

| Preview | Recorded / attempted | Buffered / durable fps | Encode median ms | Late completions | Preview count / median render ms |
|---|---:|---:|---:|---:|---:|
| Disabled | 888 / 900 | 29.60 / 28.44 | 28.46 | 413 | — |
| 512x384, background | 875 / 900 | 29.16 / 28.13 | 33.05 | 871 | 292 / 54.42 |
| 256x192, background | 890 / 900 | 29.67 / 28.65 | 30.67 | 528 | 297 / 3.87 |

All requested previews rendered. The smaller preview has much lower processing
cost. Differences in accepted counts include scheduling/storage variability;
they do not establish a capture speed benefit from enabling preview.
Capture-to-sink p95 was 196.52/138.33 ms at 512x384/256x192. Peak RSS was about
510.7/534.8/521.7 MB; final flush took 1.23/1.10/1.07 seconds. Late completion
means write completion more than one nominal frame period after actual submit;
nominal sensor scheduling jitter is not separately measured.

An earlier 120-attempt comparison accepted 120 frames with background preview
and 114 foreground. Background rendering took longer (36.81 vs 28.52 ms median)
while yielding CPU to capture, but still had 71 late completions. The sustained
runs supersede any inference of reliable 30 fps from that short experiment.

A current 20 fps Mission 1 replay exercises the full 1024x768 preview rather
than the smaller 30 fps samples above. Parallel independent-channel reduced
decode plus fused wavelet-to-calibrated-RGB NEON processing delivers 120/120
previews with no replacement and exact offline pixels. Its 49.01 ms render
median is down from 105.11 ms and 56/120 deliveries on the prior path. The
same-frame latency gate remains open: preview begins only after encoding the
compressed payload, so capture-to-sink p95 is 99.18 ms. See the
[exact receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-parallel-preview.json).

The subsequent encoder-observer handoff offers preview before storage begins.
Its matched 120-frame Pi replay again records and previews all frames with
exact stock payloads/calibrated pixels, zero replacements and zero capture
deadline misses. Render median is 48.91 ms, capture-to-sink p95 96.50 ms and
buffered capture 20.06 fps. The roughly 700 ms final fsync lowers short-run
durable throughput to 17.96 fps. The change removes a storage dependency;
single-run differences from 99.18 ms p95 do not establish a statistical latency
improvement. Encoding plus reduced decode/render still exceeds the 50 ms
same-frame gate. [Observer receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-core-receipts/mission1-pi5-encoded-observer.json).

Neither baseline nor combined runs establish reliable live 30 fps. Both Pi HDMI
connectors were disconnected. Sensor capture, physical presentation, microSD,
CNN inference and full thermal qualification remain unverified. The governor
was performance at an observed 2.4 GHz. An environment snapshot had a historical
soft-temperature-limit flag and no current throttle flags; per-frame frequency
and temperature were not recorded throughout the runs.

## Verification

The queue test gates callbacks to prove replacement, copied payload/retained
plan lifetime, concurrent finish, cancellation, capacity rejection and sticky
sink/decoder errors. Combined tests record copied/leased inputs, force overruns
and sink failure, verify complete recordings and compare all delivered pixels
against offline playback using each recorded frame's actual calibration.
Installed C/C++ clients create/drain a queue and verify delivery after releasing
the caller's plan reference.

Real-source receipts are in `stock-capture-preview-receipts/`. Verification
checks complete CRC/END3 framing, every accepted payload, exact source
calibration and every preview's timestamp, source, payload CRC and offline RGB
CRC. Separate Q3 encodes use the earlier upstream stock build, identified by
probe/library hashes. Sanitizer/package receipts state exact scope/revisions.

## Bounded plane-placement follow-up

[Final queue placement](stock-preview-colors.md) separates copied planes within
spare reserved capacity and reports actual spaced payload usage. Tight bounds
retain dense storage; row packing, int16 alignment, CRCs and public structures
are unchanged. Matched counters reduce L1 refills, and all four final capture
runs remain exact. Deadline misses persist; this does not establish a strict
20 fps floor or physical display integration.
