# Preview row work sharing

Large wavelet previews now let four workers claim 32-row blocks from a
per-render counter. A worker that starts promptly can process more rows while
another is delayed. The calibrated pixel arithmetic, public APIs, thread limit,
input alignment requirements and buffer reservations remain unchanged. Small
previews and the separate direct-RGB display branch retain their existing path.

The atomic counter uses relaxed ordering to assign unique, disjoint output
row ranges. Future completion joins all tasks before output is consumed. The
counter and closure are declared before the futures so they remain alive while
already-launched tasks finish if launching another worker throws. Each render
has independent state; plans remain immutable and shareable.

## Worker diagnostic

A private instrumented 120-attempt replay records wall and thread CPU time
around each color worker's row loop, plus actual scheduling/affinity. All 120
payloads, calibration records, source/attempt/PTS, preview pixels/CRC and END3
pass verification before temporary clip deletion. There are 480 records; four
warm-up frames are excluded, leaving 58 observations per source/worker.

All workers are FIFO priority 1, nice 0, with all four cores available. The
first three row ranges usually start within 0.1 ms of each other. The fourth
range starts about **7.16 ms later** and finishes last. Its own CPU and wall
times are close, so the delay precedes that row loop.

| Source | API render median ms | Worker-loop envelope ms | Sum worker CPU / 4, ms |
|---|---:|---:|---:|
| GP011728 | 11.71165 | 11.59212 | 6.48912 |
| GPAB1094 | 11.46140 | 11.32820 | 6.29842 |

CPU/4 is arithmetic headroom, not a guaranteed attainable time. The cause of
the delayed start is not isolated. Instrumentation prints after each timed
loop and can perturb sibling/API timings; this is not throughput approval.
The instrumentation is retained only in the evidence archive.

## Fixed three-worker control

Reducing the fixed partition count to three does not qualify an improvement.
The fixed baseline/candidate/candidate/baseline comparison gives:

| Run | Accepted/previewed | Render median ms | Durable fps | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Four fixed workers 0 | 600/600 | 11.26890 | 19.9980 | 8 | 6 |
| Three fixed workers 1 | 600/600 | 9.61439 | 19.9964 | 42 | 38 |
| Three fixed workers 2 | 599/599 | 10.04550 | 19.9634 | 43 | 40 |
| Four fixed workers 3 | 600/600 | 9.99150 | 19.9951 | 37 | 37 |

All accepted outputs are exact, but one candidate loses an attempt and late
deliveries remain. This configuration is not adopted.

## Four workers sharing row blocks

The subsequent fixed comparison uses the same accepted baseline executable,
source inputs and policy, changing only large wavelet-preview row scheduling.
Both modes use alternating native 4096x3072 RGGB14 stock-Q3 inputs, streaming
preview each frame, serialized compute, foreground preview, leased input,
192 MiB workspace, 1 MiB rolling writeback and final fsync. Child UID 1000,
FIFO1, nice 0 and zero capabilities are explicitly observed.

| Run | Accepted/previewed | Render median ms | Durable fps | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Fixed partitions 0 | 600/600 | 11.32780 | 19.9977 | 7 | 24 |
| Shared row blocks 1 | 600/600 | 9.51589 | 19.9969 | 7 | 1 |
| Shared row blocks 2 | 600/600 | 7.58222 | 19.9977 | 19 | 15 |
| Fixed partitions 3 | 600/600 | 9.55030 | 19.9934 | 32 | 30 |

Both candidate render medians and late-preview counts are below both surrounding
controls. Recording and final-durability results remain mixed. **Every run
fails the unchanged strict combined 20 fps floor.** Final fsync takes roughly
8.7..16.4 ms in this comparison and remains part of durable rate. The result
supports the small work-sharing change; it does not establish a universal
speedup, zero late delivery or deployed camera throughput.

All 2,400 dynamic-comparison frames and all 2,399 accepted three-worker-control
frames pass exact payload/calibration/PTS/pixel/END3 verification before their
new temporary clips are removed. Persistent receipts retain clip SHA-256 and
bytes. An independent audit binds source/protocols and recomputes accounting
and deadline decisions. Pre-existing workloads remain unchanged; no new Pi
build/test/profiler runs during timed capture children. Thermal observations
are retained. These are warm-cache memory-preview/SSD replays, not sensor,
rear-display, SD or thermal-deployment validation.

## Correctness and packaging

Fresh Release validation passes 30/30 Mac full-SDK and 24/24 Pi SDK-less CTests,
8/8 capture frontend checks per host and all four relocated Mac package profiles.
The existing large-preview test now covers both 768 and 762 output rows,
including a partial final block. Below-threshold serial strips with global
coordinates form the golden complete image. Repeated parallel calls and two
concurrent callers must match every pixel and destination-padding byte. No
new test target or runtime configuration switch is introduced. Hosted results
must be evaluated on the revision introducing this source change.

[Summary](stock-preview-scheduling-receipts/summary.json),
[offline archive](stock-preview-scheduling-receipts/evidence.tar.gz) and
[member hashes](stock-preview-scheduling-receipts/manifest.json) retain source,
profiling instrumentation, protocols, raw logs, observed policy, independent
audits and package identities. Large photos, source RAWs, model weights and
media clips are excluded. Archived drivers require the original hash-bound
Pi input/profiles/reference payloads and baseline executable. Each recording
requires a new directory and checks free space; only newly created clips are
deleted after successful verification. Deleted clip bytes cannot be re-read
from these receipts.
