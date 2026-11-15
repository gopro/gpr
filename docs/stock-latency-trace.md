# Stock encode latency attribution

Two private diagnostic replays find a substantial scheduling/interrupt-time
contribution to particular long encodes. They do not identify the competing
task or establish a fix. The subsequent [affinity rotation](stock-affinity-offset.md)
fails to establish a consistent benefit. Production remains unchanged.

A [subsequent kernel-event collection](stock-kernel-scheduler.md) now identifies
MMC maintenance in comparable gaps; the findings below remain bound to their
original diagnostic runs.

## Instrumentation and scope

The private patch on `6efc5bd` records each channel worker's monotonic wall time,
thread CPU time, processor and Linux voluntary/involuntary context-switch
increments around forward transform, incremental entropy and band flushing.
The parent prints after all joins. Cumulative encoder timestamps delimit
unpacking, encoding, the reconstruction observer and cleanup. A capture-side
record supplies the frame index, full encoder-call wall time and calling
thread CPU time, with successful clock calls checked there.

This includes logging and measurement overhead. The worker wall interval also
includes the small usage-query overhead outside its CPU bracket. Wall minus
thread CPU is an estimate of time not accounted to that thread; it does not
separate preemption, interrupts and measurement costs. Context switches support
a scheduling contribution but do not assign every unaccounted nanosecond to it.
These are diagnostics, not recording acceptance benchmarks.

Both Mac/Pi private builds pass 14 codec/preview tests. Both replays use the
frozen alternating inputs, 12 MP Q3, every-frame calibrated preview, serialized
compute, 1 MiB writeback and final fsync. Verification follows child completion.

## Two observation methods

The first driver drains child stdout/stderr through Python subprocess pipes.
It accepts, writes and previews 598/600 attempts, with two recording overruns.
All 598 payloads, source calibrations, preview CRCs, associations and the end
marker verify; all deliveries are late. The slowest encode is 66.5804 ms.
Its critical channel 0 occupies 53.0790 ms wall but 17.8217 ms thread CPU,
with 36 involuntary and zero voluntary switches. The calling thread's full
encode wall/CPU difference is 39.1880 ms.

The follow-up uses the **same executable**, writing logs directly to files
while the Python parent waits. This reduces parent wakeups to drain logs;
it retains logging costs and still is not acceptance timing. It accepts,
writes and previews all 600 attempts, with exact independent payloads,
supported calibration, delivered preview CRCs and final framing. It has
335 late writes and 440 late previews.

| File-logged trace metric | Median ms | Maximum ms |
|---|---:|---:|
| Full encoder call | 40.7084 | 63.0289 |
| Calling thread CPU | 38.4675 | 45.0948 |
| Critical worker wall | 29.7175 | 51.9963 |
| Critical worker CPU | 28.4877 | 32.7326 |
| Time from last worker phase finish to completed joins | 0.0187 | 0.5832 |
| Reconstruction observer callback | 8.7738 | 9.9724 |
| Encoder cleanup | 0.0389 | 0.2945 |

The critical worker is selected by latest finish for each frame; maxima and
medians in different rows need not refer to the same frame or sum.

File-run frame 470 is the slowest encode: 63.0289 ms versus 33.3005 ms calling
thread CPU. Channel 0 uses 22.3308 ms CPU during 51.9963 ms wall, with 28
involuntary switches. Its reconstruction callback takes 8.7052 ms and cleanup
0.0554 ms. Another long encode at frame 141 has a similar roughly 29 ms
unaccounted critical-worker interval. Both observations persist without a
live parent log-draining loop.

All workers are observed on their expected default processors 0–3. In the
file run, the maximum per-channel wall/CPU differences are 29.666/3.550/3.600/
5.914 ms. This motivates testing placement; it does not prove an IRQ source
or that moving channel 0 elsewhere solves the problem. Short measured join
tails do not explain these particular long intervals.

All accepted outputs from both runs remain exact. Both strict timing gates
remain false, and normal compute plus reconstruction/rendering still consumes
much of the 50 ms budget. Removing a rare scheduling stall alone would not
establish a production floor or broader model-quality approval.

![File-logged diagnostic wall and CPU timings](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/file-trace.png)

The [plot source](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/plot-trace.py) reads the archived
correlations directly; its optional plotting dependencies are recorded in
[the environment receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/plot-environment.json).

## Evidence and next investigation

[Pipe-run protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/trace/protocol.json),
[verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/trace/receipt.json),
[file-run protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/trace-file/protocol.json),
[verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/trace-file/receipt.json),
[frame correlations](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/trace-file/correlation.json) and
[worker summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-latency-trace-receipts/file-worker-summary.json)
retain the identities and complete observations. The directory includes the
patch, build/test logs and both replay/analysis drivers. Apply the patch to
`6efc5bd`, use the archived build commands, adapt input/install paths and choose
fresh output roots. Raw binaries/media remain under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_latency_trace_20261030` and the matching
Pi directory. The local prototype's tracked source is restored to `6efc5bd`.

Next, correlate absolute worker intervals and thread identities with kernel
scheduler events to identify tasks running during the gaps. Kernel tracing
must remain a separate diagnostic, with its overhead recorded. The trace does
not justify a scheduler/priority default change on its own.
