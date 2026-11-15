# Stock RAW capture: first rebuild milestone

This replaces the fused-codec dependency of the earlier capture shim with the
optimized historical VC-5 encoder. It is part of the ongoing
[feature-suite rebuild](stock-raw-media-rebuild.md), not a completed replacement
for the entire previous feature PR.

## Build and exercise

```sh
cmake -S . -B build-runtime -DCMAKE_BUILD_TYPE=Release \
  -DGPR_ENABLE_VIDEO=ON -DGPR_BUILD_CAPTURE_APP=ON -DGPR_BUILD_VIDEO_TESTS=ON
cmake --build build-runtime --target gpr_capture_app capture_lifecycle_test \
  capture_c_api_test --parallel 4
ctest --test-dir build-runtime --output-on-failure
build-runtime/gpr_capture_app --input GP011728.raw --input GPAB1094.raw \
  --output clip.gvid --quality 3 --frames 900 --paced --queue 4 --leased \
  > capture.jsonl
python3 tools/stock_video.py inspect clip.gvid > inspection.json
```

The replay app requires POSIX. The library has a C ABI and uses C++11 threads
plus zlib; independent build profiles and Windows verification remain part
of the larger rebuild. The app preloads unpacked Bayer fixtures and validates
their sample range before timing. No sensor or camera firmware is attached.

## Input ownership and backpressure

`source/lib/gpr_video/gpr_capture.h` exposes create, submit, flush, finish,
cancel, destroy and stats. Submission indices start at zero and advance only
on success. Timestamps must strictly increase and are preserved as supplied;
the library does not replace them with a nominal frame-rate clock.

Capture requires width and height at least 48, in addition to the framing
geometry limits. Smaller configurations are rejected before starting workers.
See the [encoder boundary contract](stock-encoder-bounds.md).

By default, input is copied before submit returns, including repacking padded
rows. Metadata is always copied. Alternatively, set `frame.release` and its
user pointer to transfer a read-only input lease. Leases require tightly
packed rows. After a successful submit, keep the buffer valid and unchanged
until the release callback runs exactly once on the codec thread. This also
holds on cancellation or failure. Rejected submits retain caller ownership
and do not invoke release. The source buffer can be reused after release;
storage need not have finished with the compressed frame yet.

The lease bypasses the frontend RAW copy. An odd source address still makes
the legacy encoder use aligned scratch; it does not establish a zero-copy
codec path for every caller layout.

`max_inflight` (1–16) bounds **all** copied/leased, encoding, encoded and
writing frames together. There are no unbounded raw or storage queues. A
zero timeout tries submission without waiting for queue space; a finite
timeout bounds queue/submission-serialization waiting; `UINT32_MAX` waits
without a deadline. Ordinary short internal mutex sections still serialize
state access. The caller chooses blocking, retry or explicit overrun policy.
No source frame is silently dropped by the library.

The app's default is nonblocking: full queues count an overrun and skip that
source attempt. Accepted frame indices stay contiguous, while per-frame
`attempt_index` records skipped source attempts. `--submit-timeout-ms` changes
that policy. Replay input leases use stable preloaded fixtures and a no-op
release callback; native lifecycle tests exercise real release ownership.

The current public configurations are RGGB, GBRG, GRBG and BGGR at 12, 14 or
16 bits per sample with stock qualities 0–5. The replay app selects these with
`--cfa` and `--sample-bits`; its default remains RGGB14.
Input samples must fit the configured bit depth; per-pixel checking is an
application responsibility, not an extra hot-path scan in the library.

## Workers, callbacks and completion

One worker calls the stock encoder, whose independent Bayer channels can run
in parallel. A second worker performs framing, checksum calculation and
writer callbacks. The codec entry point now serializes its process-global
codebook setup across capture and ordinary SDK calls, while retaining
parallel work inside each encode. This is a correctness boundary, not a
promise that multiple simultaneous cameras scale linearly.

Writer bytes expire when the callback returns. Return success only after
accepting all bytes; the library cannot undo a partial external write.
Completions describe fully accepted frame writes and run outside internal
locks. Writer/completion/input-release callbacks may query stats but must not
call submit, flush, finish or destroy. Cancellation wakes queue waits and
releases queued inputs, but cannot interrupt an application callback that is
blocked in its own I/O. That callback must provide a finite wait/cancellation
mechanism if the application needs one.

Flush drains accepted frames and permits later submissions. Finish closes
submission, drains the workers, emits the end marker, and waits until both
workers have stopped. No callback occurs after finish returns, including
error returns. Destroy finishes and joins; use cancel first to discard queued
work. The application owns file close, filesystem synchronization and display
presentation. The app's `--fsync-each` and final flush are explicit measurements
of those storage choices, not library promises of durability.

An optional `gpr_capture_set_encoder_wait` callback, configured before the first
successful submission, runs after writer enqueue and input-lease release,
outside capture/codec locks. It receives a copied frame index; the writer can
finish/recycle that frame while the callback waits. Capture flush waits for an
active callback as well as inflight frames. Finish joins it, and cancellation
cannot interrupt it. Keep context alive through finish/destroy; callbacks may
query stats but must not submit/flush/finish/destroy. The app uses this with
`gpr_preview_queue_flush` under `--preview-sync-stages` to prevent preview from
overlapping the next encode while retaining writer/preview overlap. Default
scheduling is unchanged. See [measurements and limits](stock-preview-compute-stages.md).

## Allocation and optimization policy

Frame slots are preallocated and first-touched at create. Each slot reserves
`raw_bytes + 1.5 * raw_bytes + 1 MiB` plus bounded metadata; the reusable
workspace is additional. At 4096×3072 with four slots and a 192 MiB workspace,
the main reservation is about **436 MiB**, before input fixtures, stacks and
runtime overhead. Leased input currently retains the allocated copy buffers
but avoids their per-frame copies. Reducing that reservation is still work.
The app reports peak process RSS separately from workspace peak usage.

The optional arena is a preferred reusable reservation, **not a hard process
memory cap**. Allocations that exceed it use tracked malloc fallback and are
released/reset after the frame. Stats expose both arena and fallback peaks.
This avoids passing an artificially tiny arena failure into historical codec
paths that are not fully hardened for allocation failure. System-memory
failure hardening remains in the parent codec backlog.

The experimental streaming/assembly/compact-band choices still use the
inherited process environment flags. The app sets them before workers start;
library users must set them before capture and leave them unchanged during
use. Moving these choices into explicit encoder configuration remains work.

GVID checksums use ARM IEEE CRC instructions where supported (runtime HWCAP
dispatch on Linux AArch64; compiler-enabled CRC elsewhere), with zlib fallback.
Checksums remain standard IEEE CRC32. The writer combines an existing payload
checksum into the stream checksum algebraically, avoiding a second scan of
each multi-megabyte payload. CRC is corruption detection, not authentication.

## Container and recovery

The new, draft **stock-VC5 GVID v3** profile preserves actual timestamps,
clip/frame metadata and integrity checks. It deliberately differs from fused
GVID v1 and the minimal experimental stock v2 profile. No VC-5 payload bits
or stock quantizers are changed. This is not a final public format allocation.
`gpr_gvid.h` documents the exact little-endian layout:

- 64-byte clip header, bounded metadata, header/metadata CRCs;
- 48-byte frame header, contiguous index, actual timestamp, bounded metadata
  and payload, with independent header/metadata/payload CRCs;
- 48-byte END3 marker with frame count, last timestamp, total payload bytes,
  rolling stream CRC and trailer CRC.

Metadata is application-provided UTF-8 JSON. The native framing layer treats
it as opaque bytes and checks bounds, not camera schema; the Python reader
requires a valid JSON object. Full calibrated camera metadata integration is
still pending. The replay app identifies its source as fixtures and explicitly
marks calibration as externally required.

The Python reader accepts historical stock v2 and this v3 profile; it rejects
fused v1 instead of sending fused bytes to the stock decoder. V3 strict mode
requires a valid END3 and rejects truncation, corrupt bytes, wrong sequences,
nonmonotonic timestamps and trailing data. To inspect or extract a complete,
verified prefix from a failed recording:

```sh
python3 tools/stock_video.py extract interrupted.gvid --recover --out-dir recovered
```

Recovery reports `complete=false`, a reason and the verified prefix only;
it does not turn an interrupted recording into a successful capture receipt.
Clip-header/calibration corruption cannot be recovered by guessing. The
native RAW decoding and calibrated editable export now accept all 12 video
formats. The optional CNN reconstruction adapter still requires RGGB14.

## Validation boundary

Native tests cover copy/lease ownership, padded rows, timestamp and metadata
preservation, bounded waits, cancellation/release accounting, writer and end
marker errors, flush/resume/finish and concurrent capture/SDK entry points.
A C translation unit compiles/links the public interface and compares CRCs
against an independent bitwise implementation over varied lengths, seeds and
alignments. The Python suite independently validates native output and rejects
every single-byte corruption and every truncation of a two-frame fixture,
plus whole-record removal and trailing bytes. Original v2 tests remain enabled.

These are host integration tests, not camera delivery evidence. The broader
goal still includes calibrated metadata/stills/high-bit-depth workflows,
native preview, MOV/ProRes/editable outputs, model pairing and quality gates,
cross-platform packaging, and sustained hardware measurements.

## First Pi measurement for the committed implementation

Source `ea418d147980b5f91a22a5a02596ef6231b85e2a` was measured with four
in-flight slots, input leases and ARM CRC acceleration. The harness attempts
900 arrivals at 30 fps, alternates the two supplied 4096×3072 RAW fixtures,
and rejects a source arrival explicitly when the queue is full. Accepted
frame mixtures can therefore differ between runs. All accepted frames are
retained; the recorder does not hide rejected source arrivals as encoded FPS.

| Quality | Written / attempted | Source overruns | Buffered FPS | FPS incl. final flush | Median codec call | Late completions |
|---|---:|---:|---:|---:|---:|---:|
| Q3 | 895 / 900 | 5 | 29.84 | 28.47 | 25.76 ms | 327 / 895 |
| Q4 | 811 / 900 | 89 | 26.95 | 25.91 | 35.53 ms | 783 / 811 |

These are **not reliable 30 fps results**. Completion p95 was 104.8 ms at Q3
and 155.2 ms at Q4; four slots absorb only bounded backlog. Q3 final `fsync`
took 1.44 s and Q4 1.20 s. Peak RSS was 510,656,512 bytes (~487 MiB), including
both RAW fixtures; peak arena use was 97,936,896 bytes with zero fallback.
The source-frame mixture and asynchronous work make these timings different
from #100's serial, paced catch-up loop; do not interpret their FPS as a
matched architectural speedup or a frame-deadline guarantee.

All **1,706 accepted payloads** matched independent fresh stock encodes at
the corresponding quality, and frame metadata, checksums, accounting and
END3 were verified before large temporary clips were removed. The source,
binary, input and output hashes and individual frame timings are preserved in
[the Pi summaries](stock-capture-pi5/summary.jsonl), the per-frame JSONL files,
[source manifest](stock-capture-pi5/source-manifest.json) and
[environment receipt](stock-capture-pi5/environment.json). All 368 committed
source/test/tool files were independently hashed and matched on the Pi.

Reproduce with the instrumented independent reference from
[the original experiment](stock-raw-video-12mp.md):

```sh
python3 tools/benchmark_capture.py --binary build-runtime/gpr_capture_app \
  --reference "$REF" --input GP011728.raw --input GPAB1094.raw \
  --out-dir capture-results --frames 900 --queue 4 --leased
```

The initial two-scan-CRC copied-input prototype fell below 20 fps on Q4.
That led to explicit input leases, algebraic CRC combination and hardware
CRC dispatch. The default copy path remains available for caller-owned
short-lived buffers; it is not assumed to meet the lease path's throughput.
Further memory/storage scheduling, preview integration and real sensor/SD
tests remain part of the active rebuild.
