# Preview gain-table layout and rejected worker experiment

The retained change reduces calibration-table traffic without changing the
prepared floating-point values or preview math. A plan whose **evaluated final
clipping caps all equal exactly 1.0f** stores four gains per output pixel instead
of four gains plus four constant caps. Other plans retain the eight-float layout.
There is no tolerance test, quantization or changed clipping order. In
particular, a gain-2 operation followed by attenuation-0.5 has a final clipping
cap of 0.5 and must retain the original representation despite its net gain of
one. Existing independent color fixtures check that case.

For a qualifying 1024x768 plan, active gain storage falls from 24 MiB to 12 MiB.
The AArch64 four-pixel path uses one `LD4` gain load with constant caps; the
fallback uses two loads and deinterleaving. The measured binary's
[assembly](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/display-wavelet-rows.asm) and
[load-site receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/assembly.json) show these
branches. This does not claim elimination of all temporary-array/vector spills.
The scalar renderer uses the same exact stored values.

Plan preparation first evaluates the original table, then copies eligible gains
and releases that table. Its temporary allocation peak can therefore increase;
this is a reduction in retained storage and per-frame reads, not a promise of
halved process RSS or lower preparation cost. No public header, ABI, source
metadata, stock compressed bytes, color transform or default scheduling policy
changes.

## Sustained Pi comparison

The four runs use previous/compact/compact/previous order, identical 600-attempt
20 fps supplied-frame replay, stock Q3, 4096x3072 RGGB14, encoder-derived quarter
preview, the memory CRC sink, and page-aligned 1 MiB writeback plus final fsync.
The previous executable is exactly the binary from the preceding writeback
receipt. Clocks report 2.4 GHz/performance; before/after temperatures are recorded.

| Run | Recorded / previewed | Durable fps | Preview render median, ms | Preview latency p95, ms | Peak process RSS, decimal MB |
|---|---:|---:|---:|---:|---:|
| Previous A | 496 / 496 | 16.4380 | 44.3212 | 286.531 | 614.99 |
| Compact B | 524 / 524 | 17.3709 | 37.6385 | 265.602 | 599.85 |
| Compact B | 513 / 513 | 17.0190 | 37.8644 | 271.641 | 599.34 |
| Previous A | 526 / 526 | 17.4321 | 41.6847 | 271.897 | 614.99 |

Every accepted compressed payload, supported source calibration and delivered
preview pixel matches the independent historical encoder/offline preview.
All four runs overrun and fail the strict 20 fps gate. Lower rendering medians
and process peaks support retaining the smaller representation, but the mixed
throughput results do not establish a robust overall FPS speedup. RSS includes
other allocations and different queue occupancy, so it is not a direct
measurement of per-plan storage. Process CPU/fault/context-switch totals are
retained in the [complete receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/compact-abba.json).

## Rejected persistent workers

A preceding prototype replaced per-frame reconstruction/display helper creation
with persistent task groups. It passed numeric, reuse, exception-barrier and
concurrent-caller checks, but its sustained previous/persistent/persistent/previous
comparison delivered 499/509/480/509 of 600 attempts. Render medians were
44.30/44.37/46.70/42.40 ms, respectively. There was no consistent benefit.
That implementation was discarded; the current runtime retains its original
scheduling. The useful large-frame correctness test was kept.

The [rejected receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/rejected-persistent-abba.json)
and [rejected patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/rejected-persistent-workers.patch)
preserve the experiment. The patch applies to base `e1a1dda`, and reconstruction
of all four archived files was checked against their byte hashes. It is an
experimental artifact, not production code or a recommended follow-up patch.

## Background-work control

An orphaned June source-simulator/encoder pair remained alive on the Pi. Its
encoder accumulated 2.13–2.37 CPU seconds during each approximately 30-second
comparison process. A separate control temporarily stopped both processes,
ran previous and compact versions, and resumed both in a cleanup handler.
Kernel state checks confirm both returned from stopped to sleeping; neither was
terminated. The [control receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/orphan-paused-control.json)
records the commands and states.

With those processes paused, previous/compact delivered 524/571 of 600 attempts,
at 17.3723/18.9316 durable fps and 38.20/32.76 ms preview-render median. These
two ordered runs still overrun and are not a universal speedup or a closed
timing gate. Background work can contribute to the measurement conditions, but
this control does not establish it as the sole cause of the earlier variability.

## Validation and reproduction

- Independent stock linear-RGB fixtures continue to cover gains, sequential
  clipping, crops, orientations, count domains and scalar/NEON agreement.
- The added 1024x768 synthetic wavelet case stitches four serial crop renders
  and compares full parallel rendering byte for byte, including row padding.
  Repeated and concurrent immutable-plan renders exercise the large-frame path.
- [24 Mac Release tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/mac-release.txt),
  [21 Pi Release tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/pi-release.txt) and
  [four stripped/relocated package profiles](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/packages-mac.json)
  pass. No CNN changes or broad sanitizer work were part of this change.
- [Measured commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/commands.json), binary hashes,
  source/receipt [bindings](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-layout-receipts/validation.json) and
  the archived rejected patch make the comparison reviewable. Media and source
  calibration remain external, with their existing frozen identities in the
  [writeback receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/aligned-final.json).

To reproduce with caller-provided inputs, use `tools/benchmark_mission1.py` with
the explicit capture/compiler/player/reference binaries, matching RAW/metadata
pairs, `--frames 600 --fps 20 --quality 3 --preview-from-encoder --writeback-mib 1`
and a new output directory per run. Its ordinary mode includes recording-only
controls; the archived commands identify the exact combined-only comparison.
Do not treat verification of accepted frames as delivery of every attempted
frame, or relabel host replay as sensor/display/SD production evidence.
