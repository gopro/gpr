# Quantizer-specific streaming preview tables

Streaming preview now caches the final stock-dequantized signed values for
-1023..1023 separately for each channel's six highpass bands. A row clamps its
signed coefficient and performs one table lookup. Table construction uses the
existing prepared cubic map, integer quantization and asymmetric signed-16
saturation. Quantizers are validated before use; their unchanged values reuse
tables across frames, while changed values rebuild only their corresponding
tables. This changes no encoder entropy, payload or reconstructed sample math.

The fixed tables add **73,692 bytes**, increasing accounted native preview
sample/table storage from 4,843,522 to **4,917,214 bytes**. Encoder workspace
remains 97,936,896 bytes; the replay reservation stays 192 MiB. Small bookkeeping
is excluded as before. No new per-row/frame allocation, public ABI or input
alignment requirement is introduced. A caller whose explicit preview budget
fit the older storage exactly may now receive `GPR_PREVIEW_CAPACITY`; the new
storage is included in the limit check and reported buffer count.

## Correctness and final-source validation

The decoder-reference comparison checks all 65,536 signed coefficients at
seventeen quantizers, including saturation and extreme negative input. Both
the prepared-map operation and new signed lookup reproduce all 1,114,112
reference samples exactly. Public streaming tests retain the 497-case format,
geometry, preset and reused-frame matrix and packed/unaligned inputs. Added
checks change presets eight times on the same stream, compare independent
entropy-decoded planes after every reset, and test exact capacity and one-byte
under-capacity boundaries.

Final candidate checks pass **30/30 Mac full-SDK and 24/24 Pi SDK-less Release
CTests**, **8/8 capture frontend tests on each host**, and all four relocated
Mac package profiles. Cache storage is value-initialized before any possible
C++ object move; caches are filled before row lookup. Earlier prototype
measurements and source remain separately recorded. Production code differs
from the final measured source only in a constructor comment clarification.
Hosted validation must be assessed on the revision introducing this change.

## Controlled CPU comparison

A separate executable links the accepted prepared-map baseline and the new
production consumer under different symbol names. Both use the same public
encoder in one process. Eight fixed blocks per native input use
baseline/candidate/candidate/baseline/candidate/baseline/baseline/candidate,
sixteen encodes each, excluding the first measurement of each block. This
gives sixty measurements per mode/source. Stream objects persist; encoding,
normal output allocation, row callbacks and inverse finish are timed. Reset
and independent payload/plane comparison are outside timing. The probe uses
the encoder's default allocation path; capture uses its reserved arena. Absolute
times should be compared within their own protocols.

| Final source | Baseline median ms | Signed-table median ms | Reduction |
|---|---:|---:|---:|
| GP011728 | 40.95096 | 39.17413 | 4.34% |
| GPAB1094 | 34.96976 | 33.70298 | 3.62% |

Every final candidate block median is below every baseline block median on
each source. All 256 payloads and three-channel preview planes match the
independent stock reference/decode exactly. The initial probe showed larger
11.84%/8.26% reductions; its source/logs remain archived, but those are not
substituted for the final-source figures. CPU frequency, allocation/cache and
session variation limit cross-run precision. The probe does not time calibrated
color, capture pacing, queues, writes or final fsync and approves no sustained
frame rate.

## Paced native recording

The preliminary 600-attempt baseline/candidate/candidate/baseline recording
keeps both modes on streaming preview. It preserves alternating 4096x3072
RGGB14 stock-Q3 sources, preview every frame, serialized compute, foreground
preview, leased input, 1 MiB rolling writeback and final fsync. Each child is
observed as UID 1000, FIFO priority 1, nice 0 with no capabilities.

| Preliminary run | Accepted/previewed | Encode median ms | Durable fps | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Baseline 0 | 589/589 | 36.8913 | 19.5531 | 589 | 589 |
| Candidate 1 | 600/600 | 33.5153 | 19.9989 | 37 | 36 |
| Candidate 2 | 600/600 | 32.2595 | 19.9985 | 14 | 13 |
| Baseline 3 | 600/600 | 33.4489 | 19.9936 | 21 | 18 |

These overlapping paced results prompted the controlled probe; they do not
by themselves establish a repeatable gain. Two additional fixed repeats use
the final value-initialized candidate and expanded preset/capacity checks:

| Final repeat | Accepted/previewed | Encode median ms | Durable fps | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| 0 | 600/600 | 32.3323 | 19.9964 | 1 | 3 |
| 1 | 600/600 | 32.4122 | 19.9987 | 22 | 20 |

All **3,589 accepted frames** across the six runs pass exact stock payload,
source calibration, source/attempt/PTS, preview CRC/pixels and END3 checks
before temporary clip deletion. Receipts retain clip SHA-256 and bytes.
Independent audits bind source snapshots and recompute event/deadline accounting.
**Every run still fails the unchanged strict combined 20 fps floor.**

Warm-cache replay is not camera integration. The preliminary thermal snapshots
range 57.30..68.30 C with `throttled=0x80000` and no active low bits; final
snapshots are retained too. Pre-existing long-lived workloads remain unchanged.
No new Pi build/test/profiler runs during timed capture children. Sensor/DMA,
physical display/SD durability, complete isolation and thermal deployment remain
unverified. Faster encoder medians do not erase late deliveries or final-fsync
costs.

## Reproduction and evidence

[Summary](stock-stream-quant-table-receipts/summary.json),
[archive](stock-stream-quant-table-receipts/evidence.tar.gz) and
[member manifest](stock-stream-quant-table-receipts/manifest.json) retain all
118 source/protocol/log/audit/package receipt files without source photos,
model weights or media clips. Preliminary and final source snapshots are
separate. The initial extraction failed on Python's unsupported `filter`
argument before building; the corrected path validates archive member types
and paths before extraction. That error remains archived.

The capture drivers require the original protocol-bound inputs, profiles,
reference payloads, launcher and baseline executable on the Pi. Each requires
a new output directory and checks free space before recording; only newly
created clips with successful independent verification are deleted. The
same-binary driver requires the recorded compiler/static libraries and builds
the baseline under renamed symbols to avoid substituting the candidate for
both paths. All compiler commands, environment flags, source/library hashes,
raw measurements and independent audit drivers are included. Deleted clips
cannot be re-read later; the stored verification evidence retains that limit.
