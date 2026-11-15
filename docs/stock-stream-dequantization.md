# Prepared coefficient lookup for streaming preview

Streaming preview now composes the stock cubic codebook mapping and inverse
companding once when creating its existing 1,025-entry lookup. Each incoming
highpass row uses one lookup, integer multiplication and signed saturation,
instead of a conversion pass followed by decoder dequantization. Lowpass
samples remain unchanged. No encoder entropy or bitstream arithmetic changes.

The lookup reuses the existing allocation. Public APIs, ring sizes, alignment
requirements, workspace reservation and preview sample-buffer accounting do
not change. Reset reuses the immutable table. Quantization remains validated
in 1..32767; expanded magnitudes are at most 1023, so the product fits int32.
Negative coefficients retain the decoder's asymmetric -32768/+32767 saturation.

## Correctness and packaging

The new `preview_dequant_lookup` test compares all 65,536 signed coefficient
representations at seventeen quantizers against unchanged decoder arithmetic:
1,114,112 exact comparisons. Existing public streaming tests compare complete
planes with independent entropy decoding across 497 geometry/format/preset
cases, including reused frames and packed/unaligned inputs.

Fresh Release runs pass 30/30 Mac full-SDK and 24/24 Pi SDK-less CTests, plus
8/8 capture frontend checks on each host. Capture/preview/camera stripped
packages and the full Mac package pass relocated C/C++ consumer checks. The
first Mac frontend invocation accidentally supplied the decoder in place of
the preview player and failed; its log is retained alongside the corrected
invocation. No assertion or numeric tolerance was relaxed. The new target is
included in the hosted workflow's explicit build list. All seven hosted jobs
pass for source revision `654775b` in
[run 41191796628](https://github.com/dcliftreaves/gpr/actions/runs/41191796628).

## Native Pi replay

A fixed baseline/candidate/candidate/baseline comparison uses the accepted
streaming baseline executable SHA-256
`3c57b19fc28808a9d57ff3417b1ca40b026ceb8e9da8b8285c3cbd6304c14130`.
Both modes use streaming preview, alternating native 4096x3072 RGGB14 sources,
stock Q3, 600 attempts at 20 fps, calibrated preview each frame, serialized
compute, foreground preview, leased 192 MiB workspace, 1 MiB rolling writeback
and final fsync. Each child is observed as UID 1000, FIFO priority 1, nice 0,
with no capabilities. These are warm-cache replay measurements.

| Fixed run | Accepted/previewed | Encode median ms | Durable fps | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Baseline 0 | 571/571 | 37.9900 | 18.9305 | 571 | 571 |
| Prepared lookup 1 | 600/600 | 34.0009 | 19.9979 | 3 | 1 |
| Prepared lookup 2 | 600/600 | 34.9259 | 19.9985 | 29 | 28 |
| Baseline 3 | 599/599 | 37.1617 | 19.9589 | 111 | 121 |

Both candidate medians are lower than both baseline medians. Averaging the two
run medians gives 37.57585 versus 34.46340 ms, an 8.28% reduction in the
preview-inclusive encode interval; this is not an 8.28% sustained-fps claim.
All 2,370 accepted frames across all four runs pass stock payload, calibration,
source/attempt/PTS, preview CRC/pixels and END3 checks before their temporary
clips are deleted. Persistent receipts retain clip SHA-256 and byte counts.
An independent audit checks saved event accounting and recomputes deadline
results. Encoder workspace remains 97,936,896 bytes and preview sample storage
remains 4,843,522 bytes.

**Every run still fails the unchanged strict combined floor.** Candidate runs
have no overruns but retain late writes/previews and durable fps below 20.
The weaker first baseline and variation between candidate runs limit precision.
A spot observation records 63.35 C and `throttled=0x80000` (historical soft
temperature limit, no active low bits). A pre-existing 124-day-old benchmark
process was observed sleeping, with roughly one CPU-second accumulated between
observations; it was left unchanged. No new Pi build/test/profiler ran during
the timed children. These observations do not establish thermal deployment
behavior or complete machine isolation. Sensor/display/SD integration remains
unverified.

## Evidence and reproduction

[Summary](stock-stream-dequant-receipts/summary.json) and
[member manifest](stock-stream-dequant-receipts/manifest.json) accompany the
[offline archive](stock-stream-dequant-receipts/evidence.tar.gz). The manifest
binds all 52 members and the archive SHA-256. It retains candidate source,
protocol, matched Pi build options, raw event logs, launch/observed settings,
verification receipts, independent audit/benchmark drivers, Mac/Pi tests and
package identities. No source photographs, model weights or media clips are
included.

Extract into a separate directory and inspect the protocol before running the
archived benchmark driver on the Pi. It requires the hash-bound original input
RAWs, calibration, preview profiles, reference payloads, baseline executable and
scheduler launcher. Its output directory must not exist. The driver checks
free space and unlinks only newly created clips after successful verification.
The archived audit binds the candidate source bytes and saved measurements;
deleted clips cannot be independently re-read later.

The subsequent [quantizer-specific tables](stock-stream-quant-tables.md)
further cache multiplication/saturation with explicit added storage. Their
final-source CPU controls and paced recordings are separate from the results
above; strict combined timing still fails.
