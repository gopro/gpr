# Rejected prepared-band inverse architecture

A prototype combines retained-band companding and dequantization while copying
encoder coefficients into aligned intermediate wavelets. Its private prepared
inverse borrows those highpass rows directly, eliminating repeated conversions
and five scratch-row allocations per inverse stage. The ordinary entropy-decoder
entry points retain the original arithmetic. No public structure changes.

The prototype is **discarded** after measurement. Production source is restored,
and the Pi executable matches the earlier `9fc7b823…` SHA-256 byte for byte.
The existing direct final-plane reconstruction remains intact.

## Correctness and sustained result

The original padded-wavelet reconstruction and independent entropy decode
continue to match the prepared path across twelve CFA/depth formats, six
presets and odd dimensions. A new private test compares prepared/ordinary
inverses in 1080 cases: both prescales, saturation, borders, odd output sizes,
unaligned/padded rows, borrowed-buffer preservation and failure of each of the
four remaining allocations. Mac's 27 experimental Release targets and four
relocated package profiles pass; six targeted Pi tests also pass.

| Previous / prepared ABBA | Recorded / previewed | Durable fps | Encode/reconstruct median, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Previous A | 600 / 600 | 19.9993 | 39.3718 | 370 | 464 |
| Prepared B | 600 / 600 | 19.9948 | 40.5914 | 378 | 452 |
| Prepared B | 600 / 600 | 19.9904 | 40.2619 | 381 | 461 |
| Previous A | 600 / 600 | 20.0004 | 39.9962 | 357 | 467 |

All accepted payloads, source calibration and delivered preview pixels match
the frozen independent stock/offline reference. Every run retains all 600
attempts but fails the strict zero-late gate. Both prepared encode medians are
slower than both controls; reduced conversions/allocations did not establish
a workflow benefit. Used reconstruction scratch falls only about 23 KB, while
the existing reservation remains unchanged. No hardware cause is inferred.

Parameters match the preceding study: two supplied 4096x3072 RGGB14 sources,
Q3, 20 fps, leased inputs, encoder-derived preview, background priority,
stage serialization, memory CRC sink, 1 MiB aligned writeback and final fsync.
Background processes remain unchanged. These are not sensor/SD/display/CNN tests.

## Evidence and restoration

The [receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/rejected-abba.json),
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/rejected-commands.json),
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/run-rejected.py),
[five-file patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/rejected-prepared.patch),
[source identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/rejected-sources.json),
[experimental Mac tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/rejected-mac-release.txt)
and [packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/rejected-packages-mac.json)
preserve the proposal. Applying its patch to `08d62d5` reconstructs all five
files byte for byte. The extra private test is part of that archived proposal,
not a current registered production target.

The [restoration binding](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/restoration.json)
checks source equality and the exact prior Pi executable. Fresh
[26-target Mac](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/restored-mac-release.txt) and
[23-target Pi](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-prepared-inverse-receipts/restored-pi-release.txt) Release
runs pass after restoration.
