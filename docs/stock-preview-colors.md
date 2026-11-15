# Bounded preview-plane cache placement

The queue now uses an odd multiple of 128 bytes for the spacing between its
three copied wavelet planes, when that spacing fits the caller's existing
payload reservation. Tight bounds retain dense storage. There is no new
allocation/reservation limit, public structure, pixel arithmetic, codec profile,
source timestamp or scheduling option. Encoder/reconstruction code is unchanged.
The source implementation is `f197f09`.

The retained result is lower measured cache traffic. It does **not** close the
strict sustained Pi 20 fps/zero-late gate or establish a universal speedup.

## Placement, alignment and memory bounds

The Pi reports a 64 KiB/four-way L1 data cache with 64-byte lines and 256 sets,
128-byte L2 lines, and 16 KiB pages. A 4096x3072 frame's quarter-scale preview
has three 1024x768 int16 planes, each 1,572,864 bytes. Dense storage places
corresponding pixels at the same low cache-index/page offsets. The untimed
pointer diagnostic observes `[16,16,16]` modulo 16,384 before spacing and
`[16,144,272]` after spacing. These observations concern low index bits, not
full physical cache tags or proof that cache conflicts cause every late frame.

The final layout rounds the plane span to an odd number of 128-byte units.
This also avoids doing unnecessary work when the original span already has
that form. Rows remain packed. Existing int16 alignment is sufficient; the
layout does not assume every row or plane starts at a 64-byte boundary.
Encoder/inverse scratch alignment and cache-skewed intermediate pitches remain
unchanged.

Three planes use `2*stride + plane_bytes`, rather than `3*plane_bytes`. The
candidate is used only when `stride <= (max_payload_bytes-plane_bytes)/2`;
otherwise the original dense stride remains. The create-time reservation is
unchanged and no offer grows beyond it. At 12 MP the active payload uses 256
more bytes; general added spacing is at most 510 bytes when capacity permits.
Payload usage statistics include spacing. Compressed-payload storage is unchanged.

## Correctness and final packaging

The added queue test compares delivered RGB bytes with direct calibrated
rendering over 20 geometry/capacity cases, including cropped output tails and
packed rows that are not cache-line aligned. It exercises tight bounds,
source-buffer mutation/free after offer, pending cancellation, and reuse of
both slots between compressed and wavelet frames. Original queue replacement,
flush, failure and lifecycle tests also pass.

The exact final source passes **27 Mac and 24 Pi Release targets**, plus all
four Mac package profiles with relocated C/C++ clients. Stripped profiles remove
the producer before consumer validation; the full profile checks its relocated
SDK/native clients. The new test is optional under the existing preview-test
build switch. No new camera dependency, model asset or sanitizer campaign is added.

Final Pi executable: `c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`.
The previous `9fc7b823…` executable/source are preserved separately for controls.

## Cache counters and limits of the claim

The final controlled counter study links the same benchmark client object and
same final archives for both modes, replacing only the queue translation unit
with the exact `7f9215e` implementation for the control. Compiler flags, SDK/JPEG
configuration and other archive hashes match. Each process includes initialization,
one-time decode and 301 queue copies/calibrated renders/CRC deliveries. The
previous/final/final/previous order covers both frozen inputs. Counters are
user-mode, include inherited worker threads, and report 100% running time.
This separate profiling run is not acceptance timing.

| Frozen source | L1 data-refill change | User-cycle change | Instruction change |
|---|---:|---:|---:|
| 0 | -5.82% | +0.09% | approximately unchanged |
| 1 | -4.59% | -1.32% | approximately unchanged |

Values compare medians of two process-counter totals. Refills are consistently
lower; cycles are effectively flat on source 0 and modestly lower on source 1.
Do not turn refill percentages into elapsed-time speedups. The earlier matched
prototype profile found approximately 5.4%/4.0% fewer refills and 0.9%/1.6% fewer
cycles. Unprofiled isolated rendering is mixed across sources/repeats. Those
preliminary observations are retained separately and do not replace the final
controlled comparison.

## Final recording/preview comparison

Previous/final/final/previous runs use the preserved old executable and exact
final executable, 600 attempts at 20 fps each, both frozen source/calibration
pairs, leased inputs, every-frame encoder-derived preview, the memory CRC sink,
serialized compute, page-aligned 1 MiB rolling writeback and final fsync.
All **2,400 final-comparison recordings/previews** match independent historical
payloads, calibration, pixels, timestamps/source bindings and end markers.
The preceding prototype comparison independently verifies another 2,400.

| Implementation | Durable fps | Render median, ms | Late writes | Late previews | Preview p95, ms |
|---|---:|---:|---:|---:|---:|
| previous | 20.0005 | 8.7420 | 395 | 470 | 93.857 |
| final | 20.0001 | 8.6400 | 109 | 322 | 63.454 |
| final | 19.9996 | 8.5654 | 311 | 451 | 113.090 |
| previous | 19.9754 | 8.4526 | 439 | 520 | 106.448 |

Both final runs have fewer late writes/previews than both controls in this
comparison, but every run fails the strict zero-late gate. Render medians and
p95 overlap between controls/candidates. Pacing caps fps near 20; retain exact
values such as 19.9996 rather than rounding them into a passing floor. Earlier
prototype comparisons also show timing variability. The bounded placement
change is retained for measured cache efficiency; it is not a universal
latency, durability, physical display/card or thermal guarantee. Existing
background workloads remain unchanged; temperature/clock readings are observations.

## Reproduction and evidence

[Summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/summary.json),
[final source binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/validation/source-binding.json),
[Mac Release](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/validation/mac-release.txt),
[Pi Release](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/final-pi-tests-v2.log),
[packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/validation/packages-mac.json),
[reported cache geometry](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/diagnostic/cache.json),
[pointer/correctness diagnostic](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/diagnostic/receipt.json),
[isolated measurements](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/isolated/receipt.json),
[matched final build](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/matched-final-build/build.json),
[matched final counters](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/matched-final-pmu/receipt.json),
[final capture protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/final-capture/protocol.json) and
[final capture verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/final-capture/receipt.json)
retain exact scope and identities.

The receipt directory contains the independent queue benchmark, previous queue
source, build/profile/capture drivers, raw counter outputs, frame events and
separate prototype comparisons. The diagnostic prototype patch applies to
`7f9215e`; copy its two test/benchmark source files to their recorded `tests/`
locations before configuring. Experimental macros/diagnostics are not retained
in production. Checkout `f197f09` for the final implementation. Copy drivers to
fresh external output roots, adapt installation/data paths and regenerate
bindings. The capture driver uses the archived direct-output command template.

`build-matched.py` reconstructs only the prior queue object using final compiler
flags and substitutes it into a copy of the final preview archive; all other
archives/client code stay identical. `profile-matched.py` performs the fixed
counter comparison. Native media, binaries, package ZIPs and full GVIDs remain
external under `/Volumes/OWC_8TB/gpr_work/artifacts/stock_preview_color_20261030`
and matching Pi directories. Broader CNN quality/restored-tier and strict Pi
deadline approval remain open.

## CI target integration

The first hosted source run at `f332e07` fails the three native jobs because
CI builds an explicit target list that omitted `preview_wavelet_queue_test`.
CTest reports this test **Not Run**; the other tests in those jobs pass. The
Mac artifact step additionally lacks files because later package stages are
skipped. This is a workflow integration omission, not a passing hosted result.

The corrected list includes the executable and preserves CTest logs in the
Release artifacts. The exact corrected build command and new test pass locally;
all seven hosted jobs pass at corrected revision `40aa476` in
[run 41139092733](https://github.com/dcliftreaves/gpr/actions/runs/41139092733).
The [successful receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/ci-integration/passed-40aa476.json)
records that exact head.
[Failure receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/ci-integration/failed-f332e07.json),
[excerpts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/ci-integration/missing-target-excerpts.txt)
and [local target check](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-color-receipts/ci-integration/local-target-check.txt)
retain the distinction. Runtime/source measurements remain bound to `f197f09`.
