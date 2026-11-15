# Borrowed preview-plane handoff: rejected

The prototype removes the queue's three-plane copy when the producer already
waits for preview before encoding its next frame. All checked output remains
exact, but both borrowed runs have higher preview rendering medians and p95
latency than both copying controls. It is **discarded**. Neither its lease API
nor its command-line flag remains in the supported runtime.

## Architecture tested

The existing encoder-derived path reconstructs three packed quarter-scale
planes, then copies them into the bounded preview queue. At 4096x3072 input,
the combined plane copy is **4,718,592 bytes per preview**. The tested path
borrows those planes until an explicit release callback. It changes ownership
and handoff rather than compression, calibration, pixels or source pacing.

The experimental `--preview-lease-planes` requires both encoder-derived preview
and compute-stage serialization. The producer's arena remains valid while
preview runs, and the stage wait includes release completion before arena reuse.
Default copying remains available in the same executable for matched controls.
Both modes retain the same queue capacity/reservations; removing this copy is
not a claim that reserved memory falls.

The queue prototype releases admitted leases once after processing, replacement,
cancellation or failure. Failed admission retains nothing. Release callbacks run
outside queue locks, and flush/finish include releases on other threads.
Mixed copied/borrowed offers, invalid descriptors, sink/render/release failure,
release-aware flush/finish and cancellation have explicit contract checks.
These are experimental preview-enabled frontend checks, not promotion of a new
supported API. The draft's release helper also lacks a guard for the
preview-disabled replay frontend: a [syntax check](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/prototype-no-preview.json)
confirms that configuration fails to compile. Package/library consumer checks
do not cover it. The original restored frontend compiles in that configuration.
The [diagnostic](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/prototype-no-preview.txt)
preserves this separate draft limitation; it does not affect the matched
preview-enabled timing runs or current production source.

## Matched Pi result

All runs use one identical prototype executable, 600 attempted frames at 20 fps,
the same two frozen 4096x3072 RGGB14 Q3 sources/calibrations, leased capture
inputs, encoder-derived quarter preview, serialized compute, background preview
priority, memory CRC sink, 1 MiB page-aligned writeback and final fsync. The
only run-mode difference is copying versus borrowing preview planes. Existing
background processes are unchanged; no test/profiler runs concurrently on Pi.

| Handoff | Recorded / previewed | Durable fps | Render median, ms | Preview p95, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|---:|
| Copy A | 600 / 600 | 20.0000 | 8.528 | 73.765 | 150 | 329 |
| Lease B | 600 / 600 | 20.0005 | 9.881 | 84.812 | 245 | 409 |
| Lease B | 600 / 600 | 20.0002 | 9.479 | 85.183 | 141 | 342 |
| Copy A | 600 / 600 | 19.9998 | 8.922 | 69.740 | 187 | 362 |

All **2,400 recordings and previews** match the independent historical encoder
and frozen offline calibrated RGB reference, including source metadata,
timestamps, payload binding, frame accounting and final markers. Both borrowed
runs report exactly **600 lease releases**. All runs still miss deadlines; none
passes the strict zero-late 20 fps gate. Near-equal, paced durable throughput
does not prove a speedup or extra headroom. Removing the copy did not improve
this workflow, so the API/flag and implementation are not retained.

These runs do not isolate why rendering becomes slower. Cache locality,
scheduling or another platform effect must not be asserted as the cause without
additional evidence. This is SSD/supplied-frame replay to a memory sink, not a
sensor/display/SD/thermal-soak or real-time CNN qualification.

## Validation and restoration

The prototype passes 26 Mac and 23 Pi Release targets, plus three stripped
package profiles and a relocated full package with C/C++ consumers on Mac. Its public C symbol
is linked by the experimental installed-client test. After rejection, all six
changed source/test files are restored. Fresh **26 Mac / 23 Pi Release targets
pass again**, the prototype symbol is absent from the Pi preview library, and
the capture executable is byte-identical to the pre-experiment binary:
`9fc7b823a088da3d6d5f5aa5985e710e4a00e9b39b4690d526ab99f9915ba80c`.

The [archived patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/rejected-plane-leases.patch)
applies to its recorded parent and reconstructs the exact
[prototype source hashes](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/prototype-source.json).
It is research evidence, not active source. Run it in an isolated checkout with
preview enabled and adjust the external input/output paths in the
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/run-abba.py) and
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/commands.json) to reproduce.
The [complete comparison](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/abba.json) retains
source/binary identities, clocks/temperatures, CPU usage and missed deadlines.

[Prototype Mac tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/prototype-mac-release.txt),
[Pi tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/prototype-pi-release.txt),
[packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/prototype-packages-mac.json),
[restored Mac tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/restored-mac-release.txt),
[restored Pi tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/restored-pi-release.txt),
[Mac restoration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/restored-mac.json) and
[Pi restoration](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/restored-pi.json) distinguish
experimental validation from the restored production state. Media remain
external; no checkpoint or model-quality policy changes.

The documentation-only parent `a7fa861` passes six hosted jobs; its Mac job is
cancelled before executing any steps. The
[receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-plane-lease-receipts/hosted-a7fa861.json) records that
status without attributing a cause. All seven jobs previously pass at source
revision `8e1c2b6`; the fresh local restoration checks above are separate evidence.
