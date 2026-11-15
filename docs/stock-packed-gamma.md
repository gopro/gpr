# Exact packed gamma table: rejected performance experiment

The private prototype reduces the preview sRGB table from 65,536 bytes to
8,192 bytes without changing any of its 65,536 outputs. It is **not adopted**:
isolated rendering is mixed, both packed capture runs render more slowly than
both dense controls, and every capture run misses strict deadlines. Production
remains the bounded preview-plane spacing implementation at `f197f09`, with
the CI integration correction at `40aa476`.

## Representation and correctness

Each 16-input interval in the existing rounded sRGB formula spans at most one
output transition. A 16-bit word stores the interval's initial 8-bit output
and its first transition position, including the no-transition sentinel 16.
Lookup uses a word load, masking, comparison and addition. Initialization uses
the unchanged floating-point formula, not an approximation. The table is
128-byte aligned; source planes and pixel arithmetic are unchanged.

The private unit checks all 65,536 indices against the independent original
formula, all 255 transitions, alignment and concurrent first initialization.
Mac and both Pi builds pass six preview tests. The compiled packed table has
SHA-256 `56560edeeb5dfc253873e06898d3e00f33b9c3c164dd28f94dda0e3b0e37b991`.
Pi executable symbols separately establish the actual 64 KiB/8 KiB storage.
The exploratory Python proof is distinguished from native test evidence.

## Matched measurements

Both fresh Pi builds use the same compiler/source/flags except the private
gamma selection macro. They retain the production queue spacing. Fixed
dense/packed/packed/dense order is used independently for isolated rendering,
user-mode counters and full capture. Counter runs do not overlap capture.

Separate counters over 301 renders per process show approximately 30.2%/10.9%
fewer L1 data refills on the two inputs, but approximately 10.6% more
instructions. User cycles improve only about 1.6%/0.4%. These counters include
startup and one initial decode; they are not an acceptance timing result.
Isolated render medians overlap: dense 5.55–5.81 ms, packed 5.46–6.15 ms.
All 488 isolated preview results match the frozen reference CRCs.

Actual paced 12 MP Q3 recording includes every-frame 1024x768 calibrated
preview, serialized compute stages, 1 MiB writeback requests and final fsync:

| Order | Table | Durable fps | Encode median ms | Render median ms | Late writes | Late previews |
|---|---|---:|---:|---:|---:|---:|
| 0 | Dense | 19.9938 | 39.7972 | 7.99405 | 159 | 335 |
| 1 | Packed | 19.9961 | 39.1823 | 8.93490 | 160 | 308 |
| 2 | Packed | 20.0041 | 36.8922 | 8.27783 | 10 | 77 |
| 3 | Dense | 20.0006 | 39.0111 | 7.92294 | 42 | 229 |

All 2,400 attempts are recorded and previewed. Independent verification checks
every payload, supported source calibration, delivered preview pixel CRC and
end marker. There are no replaced/discarded previews or recording overruns.
Nevertheless, all four strict recording/preview floor checks fail. One fast
packed run does not establish a repeatable improvement, and the rendering
medians favor dense storage in both comparisons. Lower cache traffic alone
does not justify introducing extra instructions into this runtime.

## Reproduction and disposition

[Build identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-packed-gamma-receipts/builds.json),
[isolated results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-packed-gamma-receipts/isolated/receipt.json),
[counter protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-packed-gamma-receipts/pmu/protocol.json),
[counter results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-packed-gamma-receipts/pmu/receipt.json),
[capture protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-packed-gamma-receipts/capture/protocol.json) and
[capture verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-packed-gamma-receipts/capture/receipt.json)
retain exact commands, source/binary hashes and scope. The receipt directory
also contains the patch, private header/tests, build driver and raw outputs.
Apply the patch to `40aa476`, copy the three private source files to the paths
named in the patch, and adapt the recorded external input/output paths before
running the drivers. The private benchmark and selection macro are not public
features. Large binaries/media remain under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_gamma_packed_20261030` and the matching
Pi experiment directory. This experiment changes neither model quality nor
the unmet strict sustained timing gate.
