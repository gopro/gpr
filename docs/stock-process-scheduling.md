# Explicit Linux scheduling profiles on the Pi

The unchanged stock capture executable reproduces a strict 600-frame recording
and preview pass in **two fixed FIFO-profile validation runs**, at 20.0056 and
20.0065 durable fps. Every attempted frame is recorded and previewed, with zero
late writes/previews and exact independent payload/calibration/preview checks.
The first FIFO comparison run remains a failure, so broad timing reproducibility
and default-scheduler approval remain open.

A later [native 12 MP GoPro check](stock-native12-gopro-timing.md) uses the same
binary/profile with different actual camera-file inputs. Both fixed repeats
fail combined timing: 600/570 accepted frames, 32/570 late writes and 223/570
late previews. All accepted payloads/calibration/previews remain exact. The
passes below used downsampled 50 MP source images and retain only that scope.

## Controlled setup

All runs use production executable SHA-256
`c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`, source
`f197f09` with the `f1e08c8` encoder/reconstruction core. Inputs are the same
two frozen 4096x3072 RGGB14 sources, Q3, 20 fps, 600 attempts, leased input,
every-frame calibrated 1024x768 memory preview, encoder-derived bands,
serialized compute, 1 MiB writeback and final fsync. There is no camera CNN.

Each process is launched through the same controlled privilege-drop mechanism:
root selects only that child's nice or scheduling policy; `setpriv` runs capture
as UID/GID 1000 with initialized groups and a reset environment. A normal-user
launcher records inherited settings before exec. The driver independently
checks all four UID fields, nice value and scheduler policy through `/proc`/OS
queries. Effective capabilities are zero. These checks occur at startup;
there are no profilers, unrelated builds/tests or diagnostic logs during the
measured children. Verification follows child completion.

These are process-local settings and end with the processes. System real-time
limits remain 950,000 microseconds per 1,000,000-microsecond period. No kernel
worker, old benchmark, device/controller or boot setting is altered. The API
and normal invocation defaults remain unchanged.

## Ordinary-priority controls

The first block keeps background preview and compares SCHED_OTHER nice
0/-5/-5/0. The second fixes nice -5 and compares background/foreground/
foreground/background. Neither establishes a consistent improvement against
both controls.

| Block / mode | Recorded / previewed | Durable fps | Encode median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Nice 0 | 593 / 593 | 19.6581 | 40.1944 | 593 | 593 |
| Nice -5 | 600 / 600 | 20.0056 | 35.1139 | 10 | 10 |
| Nice -5 | 600 / 600 | 20.0058 | 34.3081 | 2 | 6 |
| Nice 0 | 600 / 600 | 20.0056 | 34.5209 | 7 | 8 |
| Boosted background | 600 / 600 | 20.0046 | 36.0718 | 8 | 23 |
| Boosted foreground | 600 / 600 | 20.0032 | 34.9677 | 14 | 19 |
| Boosted foreground | 600 / 600 | 20.0035 | 36.9943 | 15 | 25 |
| Boosted background | 600 / 600 | 20.0041 | 37.8991 | 42 | 210 |

The first nice-0 run has seven overruns. Every accepted payload and preview is
checked; this does not turn its dropped attempts into success. All ordinary
controls fail the strict floor. A changed process-launch environment is not
used to retimestamp older plain-launch measurements.

## FIFO comparison and fixed validation

This block keeps nice 0 and foreground preview, comparing SCHED_OTHER priority
0 against **SCHED_FIFO priority 1**, the lowest real-time priority. The actual
capture UID remains 1000 with no effective capabilities. Configuration uses
the existing `--preview-foreground` and `--preview-sync-stages` options;
foreground is part of the tested profile, not a separately proven universal
optimization. The privileged setup is explicit in the recorded commands.

| Mode | Recorded / previewed | Durable fps | Encode median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| OTHER 0 | 600 / 600 | 20.0062 | 33.6448 | 6 | 4 |
| FIFO 1 | 600 / 600 | 20.0034 | 31.7942 | 4 | 250 |
| FIFO 1 | 600 / 600 | 20.0051 | 31.0293 | 0 | 0 |
| OTHER 0 | 600 / 600 | 20.0037 | 35.5373 | 7 | 19 |
| Fixed FIFO validation A | 600 / 600 | 20.0056 | 28.9383 | 0 | 0 |
| Fixed FIFO validation B | 600 / 600 | 20.0065 | 29.1731 | 0 | 0 |

The two validation runs are separate 600-frame replays after choosing this
fixed profile; no quality, source, attempted-frame count, timestamp or deadline
criterion is relaxed. Both verification receipts independently report
`recording=true` and `preview=true`. The initial FIFO failure is retained:
its source-0 rendering median is 10.748 ms versus 9.820 ms in the passing FIFO
comparison run, with many source-0 previews near/beyond the deadline. This does
not isolate the reason for that rendering difference.

The [kernel trace](stock-kernel-scheduler.md) provides a concrete reason to test
policy: normal-priority MMC work occupies the critical core in long observed
stalls. The unprofiled FIFO comparison has lower worst encode times than its
ordinary controls, but this experiment does not establish all interference
causes or a general camera guarantee. Three FIFO runs pass, one fails. Preserve
that distinction when describing repeatability or planning further CPU margin.

## Evidence and limits

[Nice control](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-process-priority-receipts/nice-control/receipt.json),
[boosted-preview control](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-process-priority-receipts/boosted-preview-control/receipt.json),
[FIFO comparison](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-process-priority-receipts/fifo-control/receipt.json),
[fixed validation](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-process-priority-receipts/fifo-validation/receipt.json) and
[fixed-profile commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-process-priority-receipts/fifo-validation/protocol.json)
retain all runs. Each directory includes the exact launcher/driver, startup
identity records, raw events and independent verification. Adapt the UID/GID,
absolute input/install paths and fresh output root when reproducing. Optional
scheduling privilege is needed for launch; capture does not run as root.

All 8,393 accepted payloads/previews from 8,400 attempts across these controls
are exact. Default best-effort timing, broader input/session/system coverage,
physical sensor/display/SD/thermal qualification and broader CNN quality remain
open. This is SSD replay and a memory sink. Raw GVIDs remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_process_priority_20261030` and matching
Pi folders. No production codec, queue, model or default scheduling change is
adopted.
