# Rejected early channel-preview reconstruction

A separate prototype lets each encoder channel worker reconstruct its preview
plane immediately after finishing that channel's transform/streaming entropy
work. The existing path waits for all encoder workers, then launches two
additional inverse workers and reconstructs the third plane on the codec thread.
The hypothesis was that removing this barrier and the extra launches would
shorten the combined critical path without changing codec math or preview pixels.

The draft adds an experimental channel-callback entry point, keeps the original
public callback behavior, and retains upper bands until all callbacks return.
Callbacks can run concurrently, may read only their completed channel's data,
and must not reenter encoding. A final observer dispatches any missing channels
for non-worker paths. Callback results must be discarded if encoding fails.
This describes the archived prototype; none of this API is retained or advertised
as supported. Thread-start/allocation failure and complete public-API/package
qualification were not performed for a proposal rejected at the performance stage.

## Correctness and comparison

The expanded retained-band probe checks each channel callback occurs once,
compares its plane against the independent original three-wavelet inverse, and
compares the complete payload against the ordinary historical encoder. All four
probe/matrix tests pass on Mac and Pi, covering ordinary and optimized policies,
four CFA phases, 12/14/16-bit counts, six presets and odd packed-row geometries.

The isolated timing harness uses two real 4096x3072 RGGB14 Q3 inputs, a reused
encoder arena and three separate reconstruction arenas. Both baseline and
candidate run in the same executable. Every process performs 31 complete
encode-plus-three-plane reconstructions; the first is excluded from its median.
All iterations match their process reference, and independent output hashes
match the historical payloads and baseline planes for both sources.
This measures neither rendered RGB nor writing, pacing or storage durability.

Two baseline/candidate/candidate/baseline studies test a scheduling distinction:
first the early callback inherits encoder pinning; then it restores the worker's
original CPU affinity before reconstructing, matching the later stage's wider
CPU eligibility. The second study uses its own executable and controls; do not
compare raw timings across builds as a controlled affinity speedup.

| Study / source | Baseline median of run medians | Early reconstruction | Slowdown |
|---|---:|---:|---:|
| pinned / GP017517.raw | 39.501 ms | 39.998 ms | 1.26% |
| pinned / GP017348.raw | 34.151 ms | 35.866 ms | 5.02% |
| unpinned / GP017517.raw | 39.630 ms | 40.396 ms | 1.93% |
| unpinned / GP017348.raw | 34.954 ms | 35.127 ms | 0.50% |

Neither study provides a consistent isolated win. Individual differences include
platform variability and do not prove a universal regression or identify a
single hardware cause. The prototype is rejected. Starting inverse work sooner
can also overlap memory/compute demand with the remaining encoder workers;
removing a scheduling barrier does not by itself reduce elapsed work.

The protocol's encoder/plane arena budgets, sample timings and per-channel
inverse timings remain in the receipts. This is not a thermal-control study;
existing background workloads are unchanged. No full sustained replay, model
change, ASan/UBSan campaign or production package rebuild is justified for this
rejected isolated proposal. The strict Pi recording/preview deadline gate stays
open, and production source/binary remain unchanged.

## Reproduction and evidence

[Summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/summary.json),
[pinned protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/pinned/protocol.json),
[pinned receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/pinned/receipt.json),
[pinned patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/pinned/prototype.patch),
[unpinned protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/unpinned/protocol.json),
[unpinned receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/unpinned/receipt.json),
[unpinned patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/unpinned/prototype.patch),
[Mac matrix](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/mac-matrix.txt),
[Pi matrix](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/pi-matrix.txt) and
[Pi affinity-restored matrix](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/pi-unpinned-matrix.txt)
retain the measurements. Each study directory also supplies `harness.cpp` and
`run.py`. Both patches apply to `48534e0`; copy that study's harness to
`tests/channel_preview_benchmark.cpp` in a separate checkout, apply its patch,
and verify every file against `protocol.json` before rebuilding.

Configure Release with parallel channels, preview and codec tests; the Pi build
also disables SDK/still applications. Build `channel_preview_benchmark` and
`encoder_preview_probe`; run `ctest -R encoder_preview_ --output-on-failure`.
Adapt original source/output installation paths in the driver and retain the
recorded environment flags. Regenerate bindings when paths/code/data change.
Full RAW sources, output payloads/planes and binaries remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_channel_preview_20261030` and the
corresponding Pi experiment directory. Archived callback/test changes are not
production feature additions.

## Refreshed production profile

After the isolated experiments finish, a separate 120-attempt production replay
samples user cycles at 499 Hz. All 120 recorded payloads/calibrations and preview
pixels match the independent historical/offline references, with a verified end
marker. The executable remains the original `9fc7b823…`; no prototype is used.
There are 11,065 recorded samples and no lost samples.

The flat profile assigns 25.07% of sampled cycles to incremental highpass
encoding, 19.08% to channel unpacking, 10.42% to spatial inverse and 9.61% to
wavelet display rows; memcpy is 3.08%. These are CPU sample shares, not wall-time
savings. They keep entropy packing and unpacking ahead of further queue-copy
work as investigation targets. The earlier rejected small-code pair-table
proposal remains rejected; this profile does not validate a replacement.

[Command](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/command.json),
[identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/identities.json),
[flat profile](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/flat.txt),
[call graph](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/callgraph.txt),
[perf log](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/perf.stderr),
[capture events](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/capture.jsonl) and
[independent verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-channel-preview-receipts/current-profile/verified.json)
retain the diagnostic. `perf.data` and the GVID stay in the external Pi profile
directory named by the command. Profiled latency/fps are not an unprofiled
performance comparison and do not establish the strict deadline floor.
