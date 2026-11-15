# Rejected shared unpack-row cache

A separate opt-in prototype computes all four Bayer/log components together and
shares a bounded row cache among the four encoder workers. This reduces ten
log-table lookups per Bayer quad to four when each row is computed once. It
preserves existing arithmetic, cache-skewed transform scratch and 64-byte data
alignment. Rows are copied under per-slot mutexes; evicted rows can be computed
again, with no cross-worker rendezvous or dependency on successful thread
creation. Allocation/initialization failure leaves the original path available.
This is a design description, not an exhaustive failure-path qualification.

## Isolated Pi comparison

Two real 4096x3072 RGGB14 sources are compared baseline/cache/cache/baseline,
31 reused-arena encodes per process, discarding the first sample from each
median. Both modes use the same executable; the cache is explicitly opt-in.
All eight output payloads match their source's baseline byte for byte.
The comparison measures encoder wall time, not recording/preview deadlines.

| Source | Baseline median of run medians | Eight-row cache | Slowdown |
|---|---:|---:|---:|
| GP017517.raw | 23.808 ms | 29.561 ms | 24.16% |
| GP017348.raw | 23.626 ms | 26.470 ms | 12.04% |

The eight-row data reservation adds 131,200 bytes in the Mac arena observation,
including alignment bookkeeping. An initial Mac check also produces the same
payload, but its three iterations do not establish a performance result.

## Reuse diagnostic and decision

A separate instrumented build checks capacities 8, 32 and 128 on both sources,
once per combination. Every case records 6,144 row requests and exactly 1,536
computations: ideal four-consumer reuse. All six output payloads agree for each
source. Larger capacities therefore do not recover missing reuse in these
checks. Their cold one-shot timings are diagnostic only, not controlled
comparisons against the earlier uninstrumented executable.

The eight-row proposal is rejected because both repeated comparisons are
slower despite ideal reuse. Mutex/coherence traffic, row copying and serializing
shared row computation are possible costs; this study does not isolate their
individual contributions. Fewer table lookups alone do not establish a win.
No production encoder/capture source or default changes, no packages are
replaced, and the strict sustained Pi timing gate remains unmet. Existing
background workloads are left unchanged; this is not a thermal-control study.

## Reproduction and evidence

[Summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/summary.json),
[eight-row protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/eight-row/protocol.json),
[eight-row measurements](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/eight-row/receipt.json),
[prototype patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/eight-row/prototype.patch),
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/eight-row/run.py),
[diagnostic protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/diagnostic/protocol.json),
[diagnostic measurements](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/diagnostic/receipt.json),
[instrumented patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/diagnostic/prototype.patch) and
[diagnostic driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-shared-row-receipts/diagnostic/run.py) retain source,
binary, input/output identities and full logs. Both patches apply to `b9d1c36`;
reconstructed file hashes match the respective measured protocols. Apply in a
separate checkout, configure Release with parallel channels, build
`vc5_encoder_app`, then adapt recorded data/output paths in the driver.
SDK/still apps were disabled for the Pi experiment build. Compile/run with
`GPR_STREAM_UNPACK`, band streaming, direct/compact bands, channel assembly,
reused arena and pinning as recorded in each command's environment.
Native binaries and RAW sources remain external under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_row_cache_20261030` and the corresponding
Pi experiment directory. Full capture/package/ownership qualification is not
claimed for a prototype rejected at the isolated-performance stage.
