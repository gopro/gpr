# Preview priority on the current production executable

Foreground preview does not establish a consistent benefit after the retained
direct-output reconstruction and bounded preview-plane spacing changes.
Defaults remain unchanged. All 2,400 attempts are recorded and previewed with
exact payloads/calibration/preview CRCs, but every strict timing gate fails.

The [earlier priority comparison](stock-preview-compute-stages.md#priority-control)
preceded those source changes and accepted only 557–565 frames per run. This
new comparison uses the unchanged production Pi executable SHA-256
`c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`, source
`f197f09` with encoder/reconstruction core `f1e08c8`. The four primary runtime
files are checked against local published source hashes before running.

## Protocol and results

Fixed background/foreground/foreground/background order, 600 paced 12 MP Q3
frames each, every-frame 1024x768 calibrated preview, encoder-derived bands,
serialized compute stages, 1 MiB writeback and final fsync. The only command
difference is `--preview-foreground`. All inherited `GPR_` environment settings
are removed. Background application and requested policy are checked in the
summary. No Pi builds, tests or profiling overlap these measured children;
payload/preview verification occurs after each child returns.

| Priority | Durable fps | Encode median ms | Render median ms | Preview p95 ms | Late writes | Late previews |
|---|---:|---:|---:|---:|---:|---:|
| Background | 19.9720 | 39.4501 | 8.75303 | 86.5388 | 337 | 461 |
| Foreground | 20.0032 | 37.1941 | 9.13633 | 57.3362 | 142 | 245 |
| Foreground | 20.0015 | 38.9198 | 8.52166 | 70.4410 | 160 | 292 |
| Background | 20.0043 | 35.9492 | 9.23085 | 49.6182 | 11 | 17 |

Both foreground runs improve late-delivery counts relative to the first
background run, but are worse than the final background control. The result
does not support changing the default or attributing the variation solely to
priority. The last background run's preview p95 is below 50 ms, yet 17 previews
and 11 writes are late. Neither p95 nor a durable average above 20 fps closes
the zero-late timing gate. The thermal/clock/governor snapshots are recorded;
they do not identify the cause of run-to-run timing variation.

Independent verification checks each payload against frozen stock bytes,
source calibration and frame/source associations, every delivered preview CRC,
and the final marker. Each run accepts/writes/renders all 600 attempts with no
recording overruns or discarded/replaced previews. These remain memory-sink
and replay measurements, not physical display/sensor/SD qualification.

[Protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-current-priority-receipts/capture/protocol.json),
[verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-current-priority-receipts/capture/receipt.json),
[source bindings](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-current-priority-receipts/expected-sources.json) and
[driver](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-current-priority-receipts/benchmark.py)
retain exact commands and scope. Use the recorded executable/source identities,
adapt the external data/install paths and choose a fresh output root to repeat;
the driver refuses to overwrite an existing protocol. Media remain external
under `/Volumes/OWC_8TB/gpr_work/artifacts/stock_preview_priority_current_20261030`
and the matching Pi folder. No production source or model changes are adopted.
