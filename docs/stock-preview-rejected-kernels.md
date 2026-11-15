# Rejected dequantization and row-address experiments

Neither prototype below is retained in the runtime. Production source is
restored to `f1e08c8`, including the direct final-plane reconstruction and the
optional compute-stage barrier. The strict sustained 20 fps/zero-late gate
remains unmet. Archived patches are experimental artifacts, not recommended
changes or new supported build options.

## Profile after direct final-plane reconstruction

A separate 120-attempt serialized Pi replay records 9,207 user-cycle samples.
`InvertSpatialQuantDescale16s` accounts for 9.75%, display rows 11.57%, and
`__memcpy_generic` 4.52%. The prior diagnostic reported memcpy at 11.75%.
These proportions identify remaining work; different total sample counts and
startup/workflow costs prevent interpreting the percentage difference as an
equivalent wall-time speedup. GCC already emits vector instructions in both
spatial and horizontal inverse functions; this is not a wholly scalar decoder.

The [flat profile](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/direct-profile-flat.txt),
[call graph](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/direct-profile-callgraph.txt) and
[measured command/binary identity](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/direct-profile-command.json)
preserve the diagnostic. Timed comparisons below run without profiling.

## Rejected bounded-token NEON dequantization

The prototype replaces floating-point companding evaluation for magnitudes
0..255 with a 256-entry exact table, then multiplies and saturates eight signed
coefficients in NEON registers. Larger magnitudes, unsupported quantizers,
scalar tails and partial buffer overlap retain scalar behavior. In-place rows
remain supported; loads/stores require only the existing PIXEL alignment.

An independent unchanged single-value polynomial checks every signed stock
token -255..255 at every quantizer 0..32767 (16,744,448 pairs), all 65,536
16-bit inputs at quantizer one, larger/negative quantizers on bounded tokens,
tails, unaligned rows, in-place operation and partial overlap. This test and
the existing retained-band probe/matrix pass on Mac and Pi. All 27 Mac Release
targets and four stripped/relocated profiles also pass for the prototype.
The extra oracle test belongs to the archived experiment; it is not registered
in the restored production build.

| Previous / NEON ABBA | Frames / previews | Durable fps | Encode/reconstruct median, ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|
| Previous A | 600 / 600 | 19.9997 | 39.8835 | 511 | 545 |
| NEON B | 600 / 600 | 20.0015 | 39.4092 | 295 | 400 |
| NEON B | 600 / 600 | 20.0011 | 39.5658 | 391 | 490 |
| Previous A | 600 / 600 | 19.9895 | 39.8802 | 165 | 331 |

All accepted payloads, calibration and delivered preview pixels match the
independent stock/offline reference. Both policies retain every attempted
frame. The paced producer caps throughput near 20 fps; its tiny fps differences
do not demonstrate additional compute headroom. Late-write/preview counts
overlap, and the later control has fewer late deliveries than either NEON run.
The small encode-median reduction does not establish a consistent end-to-end
benefit. The added table/dispatch path was therefore discarded.

[Receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant-abba.json),
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant-commands.json),
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/run-rejected-dequant.py),
[patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant.patch),
[source identities](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant-sources.json),
[Mac test output](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant-mac-release.txt),
[Pi oracle replay](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant-pi-oracle.txt) and
[package results](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-dequant-packages-mac.json)
retain the experiment. The patch applies to `f1e08c8`; reconstruction of all
three candidate files was checked against their byte hashes.

## Rejected per-row orientation/address preparation

A second prototype prepares destination origin, stride and reversal once per
row, replacing the orientation switch for every output pixel. It changes both
the scalar and NEON display paths, while preserving unsigned coordinate
bounds, odd row pitches, color arithmetic and the existing aligned source
buffers. Independent 576-configuration preview checks, capture/preview tests,
all 26 Mac Release targets and four relocated package profiles pass.

| Previous / row ABBA | Frames / previews | Durable fps | Render median, ms | Preview p95, ms | Late previews |
|---|---:|---:|---:|---:|---:|
| Previous A | 600 / 600 | 20.0008 | 7.6728 | 62.456 | 306 |
| Row B | 599 / 599 | 19.9018 | 8.0245 | 179.526 | 444 |
| Row B | 600 / 600 | 20.0014 | 7.6514 | 56.922 | 135 |
| Previous A | 600 / 600 | 19.9866 | 8.3073 | 91.089 | 413 |

Every accepted payload/preview remains exact, but the first prototype run
overruns and neither repeat meets the timing gate. A separate display-only
ABBA check, 400 renders per invocation, excludes decoding/setup, RGB CRC,
encoding, queues and storage. Its render medians are **4.6875 / 5.1437 /
4.7929 / 4.5462 ms**: both prototype repeats are slower than both controls,
with exact source CRCs. This implementation was discarded too.

The isolated baseline executable is byte-identical to the earlier canonical
experiment's baseline (`5523bc3b…`), which measured 6.21–6.25 ms then.
Cross-session differences must not be presented as code speedups or assigned
to a hardware cause without further evidence.

[Sustained receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-abba.json),
[commands](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-commands.json),
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/run-rejected-row.py),
[isolated receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-kernel.json),
[isolated driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/run-rejected-row-kernel.py),
[harness](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/kernel_bench.cpp),
[patch](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-address.patch),
[verified source](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-sources.json),
[Mac tests](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-mac-release.txt) and
[packages](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/rejected-row-packages-mac.json) are retained.

Both sustained experiments use the frozen two-source 4096x3072 RGGB14 Q3
replay: 600 attempts paced at 20 fps, leased inputs, encoder-derived preview,
background preview priority, optional stage serialization, the memory CRC sink,
page-aligned 1 MiB writeback and final fsync. Background processes remain
unchanged. Media remain external. These are not sensor/display/SD/CNN tests.

## Restoration and hosted validation

The [restoration receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/restoration.json) binds
restored runtime source and binary identities plus fresh Mac/Pi test output.
Neither experimental test count nor experimental package receipt is relabeled
as a current production result.
The restored Pi executable matches its prior SHA-256 byte for byte; fresh
[26-target Mac](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/restored-mac-release.txt) and
[23-target Pi](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/restored-pi-release.txt) Release runs pass.

At `f1e08c8`, six hosted jobs passed. Mac acquired no runner and executed no
steps: GitHub reports runner allocation failure and ARM64 capacity constraints
in its [annotations](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/mac-capacity-annotations.json).
The terminal [first attempt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-preview-row-receipts/hosted-attempt1.json)
is preserved. Only after that terminal failure was the Mac job retried; no
source change or reduced validation was used to evade the missing gate.

A subsequent [prepared-band inverse architecture](stock-prepared-inverse.md)
also preserves exact pixels but is discarded after slower sustained
encode/reconstruction medians. Its independent correctness and restoration
evidence is recorded separately.
