# Current runtime with the orphan workload active and paused

The current direct-output executable is tested in active/paused/paused/active
order against the same two orphaned June source-generator/encoder processes.
All runs attempt 600 supplied 4096x3072 RGGB14 Q3 frames at 20 fps, with leased
inputs, encoder-derived quarter preview, background preview priority, compute
stage serialization, the memory CRC sink, 1 MiB page-aligned writeback and final
fsync. There is one identical executable, SHA-256 `9fc7b823…`; source, pixels,
attempt count and deadline definitions are unchanged.

| Old workload | Recorded / previewed | Durable fps | Preview p95, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Active A | 600 / 600 | 19.9570 | 152.637 | 583 | 591 |
| Paused B | 600 / 600 | 19.9984 | 53.204 | 114 | 305 |
| Paused B | 600 / 600 | 20.0023 | 52.871 | 66 | 278 |
| Active A | 600 / 600 | 20.0006 | 67.988 | 183 | 358 |

Every payload, source calibration and delivered preview pixel matches the
independent stock encoder/offline reference. All frame/end-marker and source
associations are checked. Paused repeats have lower preview p95 than both
active controls here, but both still miss deadlines. Neither recording nor
preview passes the strict zero-late 20 fps gate. Active controls also vary
substantially. The old workload can contribute to latency; this does not
establish it as the sole cause or turn the controlled result into a passed
production floor.

The control verifies process command/start identity and parent-child
relationship, opens pidfds, temporarily stops both thread groups, observes all
threads stopped and checks zero CPU ticks across each timed pause. A cleanup
handler resumes them immediately after recording, before media verification;
states confirm both are running/sleeping afterward. Neither process is killed
or left paused. Pausing/resume setup is outside recording timers. Temperature,
clock/governor, identities and before/after states are retained in the
[receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-background-control-receipts/abba.json), with
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-background-control-receipts/commands.json) and
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-background-control-receipts/run-control.py).

This is a supplied-frame replay to SSD and a memory sink. Sensor, rear-display,
actual SD, thermal-soak and CNN performance are separate unverified scopes.
No runtime, default scheduling policy or persistent process setting changes.
