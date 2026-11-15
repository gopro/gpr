# Direct final-plane preview reconstruction

Encoder-derived preview now runs its final inverse transform directly into the
packed, caller-owned output plane. Previously it allocated a four-band final
wavelet, wrote its lowpass band, then copied that band row by row into a second
packed buffer. The three final highpass bands were unused. Intermediate
wavelets retain their existing 64-byte alignment and cache-skewed pitches;
quantization, inverse arithmetic, clipping, ownership and output layout are
unchanged. Ordinary entropy decoding is unchanged.

The final inverse uses the existing `TransformInverseSpatialQuantArray` entry
point with the declared packed row pitch. It does not assume every packed row
is 64-byte aligned. No public structure, preset, source timestamp, default
scheduling policy or compressed payload changes.

## Why this change

A separate 120-attempt serialized Pi diagnostic sampled user-mode cycles with
`perf record -e cycles:u -F 499 -g`. It recorded 10,530 samples without lost
samples. `__memcpy_generic` accounts for 11.75% of sampled cycles, including
9.42% under retained-band reconstruction. These are sampled CPU proportions,
not wall-time savings or a proof that every copy has the same cost. Startup and
the supplied-frame workflow are included. The measured baseline build ID
`9f9a7798f621e106b26613e5f1ae19afc7378ffe` matches the preserved executable.

The [flat profile](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/profile-flat.txt),
[call graph](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/profile-callgraph.txt) and
[command/binary identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/profile-command.json)
identify this diagnostic. Timing comparisons below run without profiling.

## Matched Pi result

Previous/direct/direct/previous runs each attempt 600 frames at 20 fps. Both
binaries use stock Q3, 4096x3072 RGGB14, two frozen source/calibration pairs,
leased inputs, quarter-scale encoder preview, background preview priority,
`--preview-sync-stages`, the memory CRC sink, 1 MiB page-aligned rolling
writeback and final fsync. Background processes remain unchanged.

| Implementation | Recorded / previewed | Durable fps | Encode/reconstruct median, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Previous A | 552 / 552 | 18.3280 | 44.6460 | 552 | 552 |
| Direct B | 600 / 600 | 19.9752 | 40.2922 | 435 | 491 |
| Direct B | 600 / 600 | 19.9988 | 40.0539 | 182 | 343 |
| Previous A | 555 / 555 | 18.3873 | 44.1469 | 555 | 555 |

Mean durable fps increases approximately 8.9% in this comparison. Both direct
runs accept all 600 attempts, unlike both controls. Accepted input mixes can
differ after overruns, so this is not identical admitted work or a universal
speedup. **Neither run passes the strict sustained 20 fps/zero-late gate.**
Do not round 19.9988 fps into a passing result. End-to-end preview p95 remains
135.40/70.94 ms for the direct runs. The explicit serialization wait remains
inside elapsed recording/delivery/durability measurements.

Peak used reconstruction scratch falls from 30,567,264 to 11,102,288 bytes,
approximately 63.7%. The application's first-touched reservation remains
40,894,464 bytes. This is reduced scratch usage and traffic, not a claim that
reserved memory or total process RSS falls by the same amount.

Every accepted payload is checked against the independent historical stock
encoder. Calibration, all delivered preview pixels, timestamp/source bindings,
frame accounting and the final end marker match the frozen offline reference.
The [complete receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/direct-abba.json) retains
binary hashes, temperatures/clocks, process CPU totals and missed deadlines;
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/commands.json) and the
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/run-abba.py) make the comparison repeatable
with the external frozen inputs.

## Scheduling follow-up with the new binary

Same-executable overlapped/serialized/serialized/overlapped runs with direct
output confirm that eliminating the copy does not eliminate compute contention.
All other replay parameters are unchanged:

| Policy | Recorded / previewed | Durable fps | Preview latency p95, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Overlapped A | 548 / 548 | 18.1698 | 263.371 | 548 | 548 |
| Serialized B | 600 / 600 | 19.9895 | 90.099 | 211 | 366 |
| Serialized B | 600 / 600 | 19.9417 | 104.685 | 376 | 473 |
| Overlapped A | 540 / 540 | 17.9206 | 260.212 | 539 | 540 |

Every accepted payload/preview still matches the independent reference. Both
additional serialized runs retain all 600 attempts, but none meets the strict
floor. Serialization stays opt-in, and the overlapping default is unchanged.
The [receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/policy-abba.json),
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/policy-commands.json) and
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/run-policy.py) preserve this comparison.

## Correctness and packaging

The retained-band probe still compares public full/channel reconstruction to
the original three-wavelet implementation and independent entropy-decoded
quarter-scale planes. New matrix targets cover four CFA phases, 12/14/16-bit
inputs and all six presets at 50x50 and 130x98. Their packed output widths are
13 and 33 samples, exercising odd row pitches, inverse borders and tails.
Both ordinary and optimized encoder policies run this matrix. Existing
compressed-byte comparisons remain exact.

[Mac](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/mac-release.txt) and
[Pi](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/pi-release.txt) Release output and
[package receipts](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/packages-mac.json) cover 26 tests
on Mac, 23 on Pi and four stripped/relocated profiles. The
[validation binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-direct-plane-receipts/validation.json) records source
hashes and Pi validation. No CNN or sanitizer expansion is part of this change.

The [subsequent kernel experiments](stock-preview-rejected-kernels.md) test
bounded-token NEON dequantization and per-row destination addressing. Neither
provides a consistent end-to-end benefit, and isolated row-address rendering
is slower. Both were discarded; their correctness, timing and patches remain
archived separately. The direct-output runtime remains intact.

## Current workload control

A subsequent [active/paused/paused/active control](stock-background-control.md)
uses the identical executable and verifies all 600 recordings/previews in each
run. Pausing the old source-generator/encoder workload reduces preview p95,
but both paused runs still miss write and preview deadlines. The processes
are resumed afterward. This narrows the latency investigation without closing
the strict zero-late gate or changing runtime defaults.
