# Kernel attribution of long Pi encodes

Kernel scheduler events identify `kworker/0:0` as the task occupying CPU 0 in
the largest observed worker gaps. A second collection with workqueue events
attributes comparable intervals to `mmc_rescan`. These are diagnostic findings;
the [uninstrumented scheduling controls](stock-process-scheduling.md) separately
test the unchanged production executable.

## Collection and validation

The private patch on `cade228` adds OS thread IDs and absolute monotonic worker
intervals to the [earlier CPU/wall diagnostic](stock-latency-trace.md). Mac/Pi
Release builds pass 14 codec/preview tests. Capture runs as UID 1000 with zero
effective capabilities, SCHED_OTHER and nice 0. Root is used only for the perf
collector and reading kernel symbols, not for the camera application.

An owned collector uses acknowledged FIFO enable/disable commands, monotonic
timestamps, a bounded lifetime, requested CPU-3 affinity and nice 10. Its normal
path finishes naturally with exit 0. The initial lifecycle proof expected zero
after an explicit stop, but perf propagated the requested SIGTERM; that failed
proof remains distinct from the successful natural-completion proof. Both
capture and collector terminal statuses are recorded, and owned FIFOs are
removed. No unrelated process is stopped.

The scheduler collection records switch/wakeup events; the follow-up adds
workqueue queue/start/end events. Both have **zero lost records and zero
per-CPU switch continuity mismatches**. All 4,784 recorded worker windows have
complete scheduler coverage. The collector named `perf` occupies 140.96 ms and
135.35 ms respectively, on CPU 3 during capture. Profiling and logging overhead
remain; these are not acceptance timing runs.

The first replay accepts 596/600 attempts; the second accepts all 600. All
1,196 accepted payloads, supported calibration, delivered preview CRCs,
source/frame associations and end markers verify against independent references.
Both diagnostic timing gates fail. No performance floor follows from them.

## What occupies the worker gaps

| Collection / frame | Critical worker wall ms | Thread CPU ms | Scheduled out ms | Main competing task/function |
|---|---:|---:|---:|---|
| Scheduler / 146 | 61.518 | 21.155 | 40.290 | `kworker/0:0`: 40.290 ms |
| Scheduler / 470 | 46.085 | 18.434 | 27.560 | `kworker/0:0`: 27.243 ms |
| Workqueue / 158 | 49.680 | 20.900 | 28.711 | `mmc_rescan`: 28.286 ms |
| Workqueue / 486 | 44.600 | 23.369 | 21.213 | `mmc_rescan`: 21.206 ms |

For scheduler frame 146, total encode wall time is 72.766 ms. The critical
worker is actually scheduled for 21.228 ms, only 0.072 ms above its measured
thread CPU time. The roughly 40 ms gap is scheduled time in the competing
kernel worker, not additional VLC instructions executing on that thread.

In the workqueue collection, callback start/end intervals and scheduled task
intervals jointly account for the `mmc_rescan` figures. Callback wall time alone
is not treated as CPU occupancy. Functions are resolved against the running
kernel's symbols; the report records kernel release and boot identity. The
work is on `events_freezable`. Queue-event current-task names can describe
interrupted context and do not prove the application deliberately queued MMC
work. Other observed contributors include reclaim, writeback and the old
benchmark process; these particular largest gaps are dominated by MMC work.

![CPU-0 scheduler timeline during the diagnostic](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/mmc-scheduling.png)

The Raspberry Pi Linux [MMC core source](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/mmc/core/core.c)
shows `mmc_rescan` checking card state and requeueing detection when polling is
required. The trace does not identify which host's detect object is involved.
This Pi boots from `/dev/mmcblk0p2`; recording targets `/dev/sda1` on the SSD.
No controller, polling, boot or system scheduler setting is changed.

## Reproduction and evidence

[Scheduler protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/kernel-trace/protocol.json),
[capture verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/kernel-trace/receipt.json),
[scheduler attribution](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/kernel-trace/attribution-summary.json),
[workqueue attribution](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/kernel-workqueue/workqueue-summary.json),
[collector controls](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/kernel-workqueue/collector.json) and
[external-data hashes](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/external-data.json)
retain scope and identities. The receipt directory contains the private patch,
drivers, parser, test logs and figure source. Apply the patch to `cade228`, use
the archived build commands and adapt the fixed installation/input paths.
Collector decoding uses the payload thread IDs even where perf's exited-task
header label is unknown; address and named work-function forms are both handled.
The raw capture is not restarted to repair parsing.

Raw perf files, decoded global events and media remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_scheduler_trace_20261030` and the matching
Pi folder. Public summaries exclude the global raw trace. Local tracked
prototype source is restored to `cade228`; production source remains unchanged.
