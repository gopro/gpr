# Pi recording writeback experiments

`gpr_capture_app --writeback-mib N` is an optional Linux writer policy. After a
completed frame crosses the requested interval, it requests data writeback with
`sync_file_range(..., SYNC_FILE_RANGE_WRITE)`. Ranges include only complete
system pages; the active append page stays outside the request. On this Pi the
page size is 16384 bytes. No additional frame queue or payload copy is added.

The request schedules I/O; it does not mark a frame durable. The existing final
`fsync` remains mandatory and its time remains in durable FPS. The default is
zero/disabled. Nonzero use on other platforms, intervals above 1024 MiB and
combination with `--fsync-each` fail explicitly before output creation. Errors
from writeback propagate through the writer failure path. The summary records
the interval, page size, request count and scheduled byte range.

## Measured results

These are paced warm-cache Pi 5 supplied-frame replays at 4096x3072 RGGB14,
stock Q3, 20 fps, leased inputs, four slots, 192 MiB workspace and USB SSD/ext4.
Combined runs use quarter-scale encoder-derived 1024x768 preview and a memory
CRC sink. Every accepted payload is checked against independent historical
stock encoding; every delivered preview is checked against offline calibrated
playback. No model runs in these measurements.

All combined rows below attempted 600 frames. A low accepted count is an
overrun, not a smaller requested workload. The initial policies preceded the
page-alignment refinement; the final row describes the current implementation.

| Combined run | Accepted / previewed | Durable fps | Final fsync, ms | Late writes / previews |
|---|---:|---:|---:|---:|
| Historical aligned-arena, final fsync only | 600 / 600 | 19.5166 | 761.971 | 9 / 6 |
| Per-frame fsync experiment | 600 / 600 | 20.0069 | 1.993 | 21 / 5 |
| Initial 8 MiB rolling writeback | 600 / 600 | 19.9906 | 28.789 | 5 / 4 |
| Fresh default-policy repeat | 515 / 515 | 16.9195 | 264.047 | 514 / 515 |
| Initial 1 MiB rolling-writeback repeat | 496 / 496 | 16.4555 | 6.412 | 496 / 496 |
| **Final page-aligned 1 MiB policy** | **600 / 600** | **20.0047** | **4.905** | **16 / 33** |

The final run preserves all 600 payloads/calibrations and previews, with zero
overruns or queue replacements. Preview p95 is 50.1079 ms and encode/reconstruct
median is 36.8485 ms. Final writeback schedules 1253539840 bytes through 600
requests; its page-aligned range excludes the partial append page and END3.
Recording-only reaches 20.0161 durable fps but still has one late write.
The strict combined zero-late gate remains **failed**.

These experiments support shorter final flushes on this filesystem. They do
not establish a universal throughput speedup or an optimal interval. Per-frame
fsync moves more cost into individual completions; rolling requests retain the
existing buffered-completion semantics. Actual SD and power-loss behavior were
not tested.

## Observed timing variability

The slow fresh repeats are retained in the evidence. The unmodified frontend,
rebuilt against identical libraries, reproduces the slowdown: its binary hash
is exactly the historical `d80020c...` hash. In an old/new/new/old 120-attempt
sequence with writeback disabled, accepted counts are 118/120/120/120 and
preview medians vary from 38.74 ms to 7.63 ms. This prevents attributing all
differences to the new option or claiming that the earlier fast replay is a
stable platform guarantee.

Thread-mask sampling found the expected four-core masks and pinned codec
workers. Evicting only completed task-owned GVID files from page cache increased
free memory from about 0.70 to 3.86 GB; the subsequent 120-attempt diagnostic
still overran, so that operation did not resolve the slowdown. Source RAWs were
retained. An interval `perf stat` diagnostic also substantially perturbs this
thread-creating workload and is labeled profiled, not used as an ordinary
timing receipt. No root cause or scheduling fix is claimed here.

## Evidence and reproduction

- [Final implementation replay](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/aligned-final.json),
  [per-frame fsync](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/fsync-each.json),
  [initial 8 MiB](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/writeback8-initial.json),
  [fresh default repeat](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/baseline-repeat.json),
  [initial 1 MiB repeat](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/writeback1-repeat.json).
- [Old/new/new/old binary control](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/frontend-abba.json),
  [profiled diagnostic](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/profiled-diagnostic.json),
  [cache preparation](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/cache-preparation.json),
  [subsequent diagnostic](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/cachefree-diagnostic.json).
- [Source/receipt bindings](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/validation.json),
  [Mac Release tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/mac-release.txt),
  [Pi Release tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-writeback-receipts/pi-release.txt).

```sh
python3 tools/benchmark_mission1.py \
  --capture /path/to/gpr_capture_app --compiler /path/to/gpr_preview_profile \
  --player /path/to/gpr_preview_app --reference /path/to/historical/vc5_encoder_app \
  --input source0.raw --metadata source0.json \
  --input source1.raw --metadata source1.json \
  --frames 600 --fps 20 --quality 3 --downsample 4 \
  --preview-from-encoder --writeback-mib 1 --out-dir /new/run
```

Omit `--writeback-mib` for the default-policy control; use `--fsync-each`
separately for that policy. `--preview-foreground` exposes the existing normal
preview-priority option. `--require-floor` exits 2 when the timing gate fails;
functional verification alone does not imply that gate passed. The updated
capture test checks policy rejection, actual Linux requests, page alignment,
complete framing and unchanged payloads. No public camera-library ABI changes.
