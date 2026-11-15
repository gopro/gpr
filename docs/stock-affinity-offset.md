# Channel affinity rotation: rejected

Moving channel 0 away from CPU 0 does not establish a consistent end-to-end
benefit on the tested Pi. The private offset proposal is rejected and local
source restored. The production channel mapping, codec and preview defaults
remain unchanged; strict sustained timing remains unmet.

The [latency diagnostic](stock-latency-trace.md) finds its largest worker
wall/CPU gaps on CPU 0. That motivates a targeted placement experiment rather
than treating every long encode as additional VLC work. It does not establish
the source of preemption or an interrupt cause.

## Implemented and verified behavior

The private `GPR_CHANNEL_CPU_OFFSET` setting rotates the existing opt-in pinning
over the thread's allowed CPUs in ascending order. With offset 1 and CPUs 0–3,
channels 0/1/2/3 map to CPUs 1/2/3/0. The previous allowed mask is restored as
before. No system-wide affinity or scheduler setting changes.

Invalid, negative, overflowing or out-of-range settings fall back to offset 0.
The extra trace flag is used only in placement tests and is absent from the
capture comparison. Mac/Pi builds pass 14 codec/preview tests. Nine Pi cases
check 648 actual channel/processor/pinning observations: offsets 0–3, invalid
values and a restricted allowed mask of CPUs 1 and 3. Each case also performs
the six-quality byte/preview probe. An additional rotated full format/tail
matrix passes four CFA phases, three depths and six qualities.

## Same-executable capture comparison

Default/rotated/rotated/default order, 600 paced 12 MP Q3 frames per run,
every-frame encoder-derived calibrated preview, serialized compute, 1 MiB
writeback and final fsync. All inherited `GPR_` settings are removed; only
the rotated runs receive offset 1. There are no trace flags, other Pi builds,
tests or profilers during measured children. Verification occurs afterward.

| Mapping | Durable fps | Encode median ms | Render median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Default | 20.0054 | 33.6525 | 8.43105 | 4 | 7 |
| Rotated | 19.9994 | 38.3214 | 8.26553 | 59 | 268 |
| Rotated | 20.0000 | 39.8753 | 9.07245 | 171 | 355 |
| Default | 20.0005 | 38.6000 | 8.82979 | 150 | 307 |

All 2,400 attempts are accepted, recorded and previewed, with exact frozen
payloads, supported source calibration, delivered preview CRCs, associations
and end markers. No recording overruns or discarded/replaced previews occur.
Nevertheless every strict recording/preview floor check fails. The best
default control still has four late writes and seven late previews. A durable
average at or above 20 fps does not erase them.

One rotated run improves some metrics relative to the final default control,
but neither establishes a consistent gain against both controls, and both are
substantially worse than the first default run. No default change or public
offset feature is justified by these results. Other offsets are not qualified
for sustained performance; placement-unit success does not imply speed.

## Reproduction and disposition

[Placement checks](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-affinity-offset-receipts/placement/receipt.json),
[rotated matrix](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-affinity-offset-receipts/rotated-matrix.log),
[capture protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-affinity-offset-receipts/capture/protocol.json) and
[verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-affinity-offset-receipts/capture/receipt.json)
retain exact source/binary hashes, commands and scope. The directory contains
the private patch and build/test/measurement drivers. Apply `prototype.patch`
to `6efc5bd`, use the archived build commands, adapt paths and select a fresh
measurement directory. The prototype's local tracked source is restored to
`6efc5bd`. Binaries/media remain under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_affinity_offset_20261030` and matching
Pi directories. The production executable still has SHA-256
`c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`.
