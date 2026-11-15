# Preview retention cost and rejected storage proposals

Retaining encoder bands for preview adds **6.7–7.0 ms before reconstruction**
in a controlled, isolated comparison on the two supplied native 12 MP inputs.
Reconstruction adds roughly another 8.8 ms in that comparison. This identifies
a larger architectural target than the preceding horizontal SIMD experiment.
It does **not** establish the cause of the production fast/slow regimes.

Three storage proposals are discarded: band-base spacing, reused entropy rows,
and a revised retained-only row branch. The row proposals improve isolated
measurements, and the first improves one native capture comparison, but broader
controls do not establish a consistent workflow benefit. Production source is
restored; the Pi capture executable again matches `c576f566…` byte for byte.

## Stage diagnostics

Four fixed 120-attempt runs reuse the previously validated diagnostic executable
`ec7b0c9f…`, whose full identity and instrumentation are bound by the earlier
[scheduler protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-scheduler-receipts/kernel-trace/protocol.json).
They use the new native GP011728/GPAB1094 inputs and their actual calibration,
Q3, encoder-derived every-frame preview, compute serialization, foreground
preview, 1 MiB writeback and final fsync. There is no kernel sampling collector.
Timestamp/logging overhead remains: these are **diagnostics, not acceptance**.

All 451 accepted payloads, calibration records and preview pixels match their
independent stock/offline references. Twenty-nine of 480 attempts overrun.
Median critical-worker wall-minus-thread-CPU time is only 0.0055–0.0062 ms,
median joins are 0.043–0.050 ms, and median reconstruction callbacks are
8.31–8.62 ms. Critical workers are channel 1 or 2 in 450 of 451 frames.
One outlier has an 18.07 ms wall-minus-CPU interval; no kernel trace is collected
here to attribute it. These measurements rule against joins or scheduled-out
time explaining the **observed slow-regime medians**. All four runs remain in
that regime, so they cannot attribute the production fast/slow split.

[Protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/stages/protocol.json),
[complete receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/stages/receipt.json),
raw events/stderr and per-frame correlations retain the distinction. The
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/stages/trace_stock_native12_stages.py)
sets diagnostic flags explicitly after the normal-user launch report.

## Separate retention from reconstruction

The [harness](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/preview_retention_benchmark.cpp)
uses the unchanged production codec in three modes within one process per
source: no observer, a retained-band observer that samples one coefficient per
band, and retained bands plus ordinary three-channel reconstruction. There is
no color rendering, capture queue, pacing or writing in these measurements.

All modes share a reused 192 MiB encoder arena. A fixed twelve-block order
provides four blocks per mode; each block runs sixteen iterations and excludes
its first, leaving sixty measured iterations per mode/source. Every encode,
including warmups, matches the independent historical stock payload. Each
reconstruction matches all three planes obtained independently by entropy
decoding that payload. The tiny retained-only observer is not a full-plane scan.

| Source / production codec | Encode only ms | Retain only ms | Retain + inverse ms |
|---|---:|---:|---:|
| GP011728 | 27.192 | 34.181 | 42.965 |
| GPAB1094 | 24.844 | 31.556 | 40.446 |

The [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/baseline/protocol.json),
[receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/baseline/receipt.json) and events
preserve block medians, extrema and actual child identity. References are
SDK-decoded camera GPR counts that are already compressed, not sensor truth.
Unpaced execution can also encounter the unchanged realtime budget; medians
are not substituted for deadline results or used to hide the retained extrema.

## Proposals and isolated results

Band spacing changes retained upper-band bases by one cache line while keeping
row pitch unchanged. It preserves exact outputs but leaves almost all of the
retention gap intact, so it is discarded without a sustained replay.

The first row proposal carves three small rows per upper level out of each
worker's existing aligned scratch allocation. Forward filtering and entropy
coding reuse these rows; exact coefficients are copied into retained storage
after all three highpass bands are encoded. It uses 18,432 additional bytes in
the 12 MP encoder arena, without changing the 192 MiB reservation or adding
separate allocation calls. The revised proposal separates that logic from the
original ordinary row-processing body. These describe **archived experiments**;
no new configuration option or private structure change remains in production.

| Variant / source | Encode only ms | Retain only ms | Retain + inverse ms |
|---|---:|---:|---:|
| Band spacing / GP011728 | 26.910 | 33.833 | 42.784 |
| Band spacing / GPAB1094 | 24.846 | 31.473 | 40.127 |
| Rows v1 / GP011728 | 27.223 | 31.965 | 40.636 |
| Rows v1 / GPAB1094 | 24.918 | 29.481 | 38.107 |
| Rows v2 / GP011728 | 25.946 | 29.023 | 35.917 |
| Rows v2 / GPAB1094 | 23.944 | 25.918 | 32.643 |

These are separate executable/process measurements with the same fixed
within-process mode controls, not a proof that every cross-session difference
is caused by the changed code. In v2's GPAB1094 process, consecutive
reconstruction block medians change from 39.168 to 32.709 ms, then remain near
32.60 ms. This within-process variation is preserved in the receipt and prevents
attributing the regime solely to a different executable or initial allocation.

[Spacing](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/spacing/receipt.json),
[rows v1](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/rows-v1/isolated/receipt.json) and
[rows v2](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/rows-v2/isolated/receipt.json)
retain all cases. Both row versions also run separate fixed ABBA comparisons
on the older GP017517/GP017348 downsampled controls, using exact frozen payloads
and independently decoded planes. V1 raises an ordinary-path timing concern;
v2's older-source results remain mixed rather than proving a general win.
No original-source calibration claim follows from this codec-only control.

## Native recording and preview comparisons

Both fixed ABBA studies use 600 attempts per run, the supplied native
4096x3072 RGGB14 counts and actual calibration, stock Q3, every-frame calibrated
1024x768 memory preview, encoder bands, serialized compute, foreground preview,
1 MiB writeback and final fsync. Baseline is executable `c576f566…`; v1 is
`aab02a45…`, v2 is `fdccb3ed…`. No unrelated Pi build, test or profiler runs
during the measured children. Independent verification follows each exit.

| Study / order | Recorded / attempted | Durable fps | Encode + inverse median ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| v1 / original A | 561 / 600 | 18.6112 | 40.6200 | 561 | 561 |
| v1 / rows B | 575 / 600 | 19.0904 | 38.3929 | 575 | 575 |
| v1 / rows B | 587 / 600 | 19.4599 | 38.5471 | 586 | 587 |
| v1 / original A | 565 / 600 | 18.7411 | 40.2700 | 565 | 565 |
| v2 / original A | 592 / 600 | 19.6291 | 37.3828 | 592 | 592 |
| v2 / rows B | 581 / 600 | 19.2861 | 38.3216 | 581 | 581 |
| v2 / rows B | 579 / 600 | 19.2017 | 38.6937 | 579 | 579 |
| v2 / original A | 565 / 600 | 18.7349 | 39.4607 | 565 | 565 |

The v1 [comparison](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/rows-v1/native-abba/receipt.json)
improves on both controls, but its
[older-source control](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/rows-v1/legacy-abba/receipt.json)
shows slower ordinary encodes. The v2
[comparison](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/rows-v2/native-abba/receipt.json)
places both candidate runs between its faster and slower controls, while its
[older-source study](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/rows-v2/legacy-abba/receipt.json)
also varies. No reliable combined gain follows from the large isolated result.

All 4,605 accepted payloads, calibration packets and preview images verify
exactly, including attempt/source binding, END3 and bounded queue accounting.
There are 195 overruns across 4,800 attempts. **Every recording and preview
floor fails.** More accepted frames can produce more late deliveries; reduced
overruns in one study must not be described as zero-late performance.

## Validation, disposition and reproduction

All three proposals pass the four existing retained-band probe/matrix checks
on Pi. V1 and v2 each pass all 27 Mac Release tests and all four relocated
package profiles, including public C/C++ clients. V1's first Pi full run passes
21/24; three Python tests fail at import because system Python lacks NumPy.
Those original logs remain under
[initial setup](https://github.com/dcliftreaves/gpr/tree/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/validation/initial-missing-numpy).
Using the existing project environment makes the three tests pass, with source
and capture binary unchanged; the
[correction](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/validation/pi-python-correction.json)
records the 21 passing results and three corrected reruns. No assertion is
relaxed. V2 is rejected at the workflow-performance stage after focused Pi
checks; it does not inherit a fresh full Pi SDK result from v1.

The [stdlib event audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/analyze_stock_preview_retention.py)
recomputes all isolated mode medians and validates block order, warmups,
checks and native event accounting. Its
[summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/summary.json) covers 4,608 isolated
stock-byte comparisons and 4,608 independent plane comparisons, plus both
complete native ABBA studies. Isolated timings are not acceptance results.

The main encoder source is restored to the published version. The Pi capture
executable again matches the original complete SHA-256, and fresh restoration
checks pass **27 Mac / 24 Pi Release targets**. The
[restoration binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/restoration/restoration.json)
and [post-state](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/restoration/post-restoration.json)
retain source hashes, UID 1000, ordinary observer priority and unchanged
950000/1000000-microsecond realtime limits. Existing June processes remain
running and untouched. All diagnostic, benchmark and validation children have
finished. Model weights, camera defaults, devices and boot settings are unchanged.

The three patches apply to `779bb47`; each was actually reconstructed and
checked against its protocol's encoder source hashes. To reproduce an isolated
variant, apply its patch and the archived harness CMake patch, copy
`preview_retention_benchmark.cpp` into `tests/`, build Release and adapt the
absolute paths in the corresponding driver. The original full-profile runner
tests all four packages for v2. V1's separately archived full-only adapter skips
only the three minimal profiles already tested; its full consumer checks are
unchanged. Package digests bind the uncommitted measured source snapshots,
not a claim that the baseline commit itself contains those proposals.

Raw media, binaries and full artifacts remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_preview_retention_20261030` and the
matching Pi experiment roots. Source GPR photos and full calibration are not
redistributed in the numeric receipts. All seven hosted jobs pass for baseline
`779bb47` ([run 41164329487](https://github.com/dcliftreaves/gpr/actions/runs/41164329487),
[receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-retention-receipts/passed-779bb47.json)); the evidence
update needs its own hosted result. Rejected source/tests are not new production
features or registered tests. Broader native-input timing and useful CNN quality
remain open.
