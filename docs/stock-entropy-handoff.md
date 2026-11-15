# Bounded stage-0 entropy handoff: rejected

The [channel-balance diagnostic](stock-channel-balance.md) motivates giving
idle channel workers some of channel 0's stage-0 entropy work. This private
prototype implements that scheduling change and preserves exact payloads, but
is **rejected**: eight-row chunks are slower in all matched standalone
comparisons and mixed in full capture. A 32-row follow-up also fails to show
consistent standalone improvement. Neither variant is production code.

## Implemented mechanism

The frame owns four fixed coefficient chunks per highpass band. The transform
writes directly into the reserved chunk, avoiding a separate coefficient copy.
Publishing exposes one job per band. Workers help only after completing their
own channel. The producer also consumes jobs when buffers fill, so absent or
failed worker creation cannot deadlock it. Each band permits only one consumer
and retains row order, pending zero-run state and bitstream state across chunks.
Final band flushing occurs after the producer finishes and all jobs drain.
All helpers join before frame-owned buffers and synchronization objects die.

Eight-row storage is 196,608 bytes at the frozen 12 MP geometry; 32-row storage
is 786,432 bytes. Rows remain 64-byte aligned. A fixed 1 MiB cap and allocation
or synchronization-initialization failure select the original inline path.
There is no process-wide worker pool or public ABI change. Selection, statistics
and forced thread-creation fallback are private experimental environment flags.

## Correctness and actual participation

Both eight-row Mac/Pi builds pass 14 registered codec/preview tests. The private
probe explicitly disables handoff for the reference, then enables it for public
encoder calls. It compares bytes and independently decoded preview planes over
four CFA phases, three depths, two tail geometries and all six qualities.
Additional Mac/Pi fast probes/matrices pass all-zero, maximum, sparse and
no-created-thread cases. The Mac codec also builds with both parallel options off.

A separate same-process test prepares 49 disabled references and verifies 392
enabled encodes across four simultaneous callers, covering normal helpers,
forced no-created-thread fallback and a 32768x48 input exceeding the ring cap.
It passes on Mac and Pi for eight rows and on Pi for 32 rows. The initial wide
fixture used height 32, below the public encoder's minimum 48; its baseline was
correctly rejected. The corrected fixture and successful logs are distinct.

Sixteen-frame Pi statistics confirm actual helper participation and exact
recordings/previews. With eight rows, helpers execute 177/4,608 jobs in the
qualified smoke run; with 32 rows they execute 120/1,152. Forced fallback executes
all jobs on the producer. That correctness run uses 5 fps: forcing every encoder
thread off at 20 fps initially overloads the four-frame capture queue, accepting
12/16 attempts. The initial assertion/partial receipt is preserved, not counted
as a successful 16-frame test. Both corrected modes preserve all 16 attempts,
and both variants report zero workspace fallback.

## Eight-row full capture

The same executable is measured disabled/enabled/enabled/disabled, without
statistics or concurrent Pi tests/builds/profilers. Each run attempts 600 paced
12 MP Q3 frames, every-frame calibrated preview, serialized compute, 1 MiB
writeback and final fsync.

| Mode | Durable fps | Encode median ms | Render median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Disabled | 19.9981 | 40.2178 | 8.20662 | 274 | 410 |
| Enabled | 20.0015 | 37.9198 | 8.27900 | 26 | 162 |
| Enabled | 19.9972 | 41.0379 | 8.73927 | 347 | 442 |
| Disabled | 20.0032 | 39.2342 | 8.20155 | 90 | 266 |

All 2,400 attempts are recorded and previewed with exact independent payloads,
supported calibration, preview CRCs and end markers. All strict recording and
preview timing gates fail. Used workspace rises from 104,257,536 to 104,454,272
bytes, with no fallback. One favorable enabled run does not establish a win.

## Standalone and chunk-size follow-up

Separate public-encoder runs compare every payload to frozen independent bytes.
Each process performs 41 encodes, excluding the first five from its timing
median. Allocation/free occurs per encode; there is no preview, writer or capture
arena. Values below are the two repeated medians for each mode and source.

| Chunk rows | Source | Disabled medians ms | Enabled medians ms |
|---|---|---|---|
| 8 | 0 | 26.674 / 27.017 | 28.445 / 27.735 |
| 8 | 1 | 20.869 / 20.716 | 21.247 / 21.461 |
| 32 | 0 | 24.916 / 24.454 | 25.779 / 25.127 |
| 32 | 1 | 18.991 / 21.554 | 21.148 / 20.836 |

All 656 standalone payloads are exact. The larger chunks reduce job count
fourfold and pass the Pi codec/preview suite, concurrency and smoke checks, but
source 0 is slower in both enabled repeats and source 1 overlaps controls.
Cross-variant absolute timings are not a matched comparison. The 32-row variant
does not proceed to another full capture comparison because it fails to establish
the prerequisite isolated benefit. The measurements do not distinguish cache,
synchronization and extra bookkeeping costs; no specific hardware cause is proven.

## Evidence and disposition

[Full-capture protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy-handoff-receipts/capture/protocol.json),
[verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy-handoff-receipts/capture/receipt.json),
[eight-row standalone](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy-handoff-receipts/standalone/receipt.json),
[32-row standalone](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy-handoff-receipts/rows32/standalone/receipt.json),
[eight-row smoke](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy-handoff-receipts/smoke/receipt.json) and
[32-row smoke](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-entropy-handoff-receipts/rows32/smoke/receipt.json)
retain commands, source/binary identities and exact scope. The receipt directory
also contains tests, logs, drivers, the patch and both private header versions.
Apply `prototype.patch` to `a00ae4a`, copy the header into
`source/lib/vc5_encoder/` and the two extra C tests into `tests/`, then configure
with codec/preview tests, capture and preview tools enabled as in the drivers.
Use the alternate header for the 32-row variant and adapt external paths.

Raw media/binaries remain under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_entropy_handoff_20261030` and matching
Pi directories. The local experimental worktree's tracked files are restored
to `a00ae4a`; the private untracked source files are removed after archive
comparison. Production retains `f197f09` preview spacing and the `f1e08c8`
encoder/reconstruction core. Strict sustained timing and broader model-quality
gates remain open.
