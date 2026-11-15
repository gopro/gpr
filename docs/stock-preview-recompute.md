# Upper-band recomputation and allocation control

Both recomputation proposals preserve payloads, coefficient views and decoded
preview planes, but are slower than retained-band reconstruction. They are
rejected without a sustained capture experiment. Production encoder source,
capture binaries, model weights and defaults remain unchanged.

The allocation-only control supplies a narrower finding: reserving the full
upper-band layout without retained writes remains within about 0.1 ms of
ordinary compact encoding. The material gap is associated with actual
retained-band processing, not the reservation alone. A future streaming design
still needs to remove those writes; this experiment does not implement one.

## Proposals and equivalence

Compact encoding already retains stage-zero LL. The first proposal consumes
upper highpass rows immediately for entropy coding, then regenerates stages
one/two from that LL before exposing the existing observer. The second uses
the fused vertical filter during regeneration. Both keep the complete legacy
coefficient view, preserve quantization and add no decoder dependency or
separate allocation calls. They repeat forward work; they are not incremental
inverse transforms or row-ring implementations.

Each version passes 17 focused Release tests on Mac and Pi, including the
existing independent encoder/decoder and preview matrix. A separate contract
compares all 24 exposed bands, quantization, prescales, dimensions and strides
over twelve CFA/depth combinations, six qualities and two geometries: 144
baseline/candidate pairs per host/version, 576 pairs total. Payloads are exact.
The first contract harness incorrectly enumerated word formats by ordinal,
including packed-input enum values. It crashes on Pi before timing. Its code
and failure are retained; named format constants correct the test. The
corrected contract supplies the evidence above.

## Isolated Pi measurements

Each recomputation study uses one binary/process per actual native 12 MP
input, a reused 192 MiB arena, twenty fixed blocks across five modes, and
sixteen iterations per block. Excluding the declared first iteration gives
sixty measurements per mode/input. Normal UID 1000, SCHED_OTHER and nice zero
are verified. No realtime, boot, device or global scheduling settings change.

Encode plus three-channel inverse medians, in milliseconds:

| Version / input | Ordinary retained bands | Recompute bands | Difference |
|---|---:|---:|---:|
| Original filters / GP011728 | 43.233 | 46.680 | +3.447 |
| Original filters / GPAB1094 | 41.219 | 44.753 | +3.534 |
| Fused filters / GP011728 | 43.633 | 46.470 | +2.837 |
| Fused filters / GPAB1094 | 41.325 | 44.564 | +3.239 |

Every recomputation block median is slower than every corresponding retained
control block median. The separate allocation control uses twelve fixed blocks
and the same warmup/count policy, without any observer in its reservation mode:

| Input | Compact encode | Full reservation only | Actual retained bands |
|---|---:|---:|---:|
| GP011728 | 27.317 | 27.394 | 34.507 |
| GPAB1094 | 26.070 | 26.173 | 32.686 |

Reservation and retained modes consume the identical 104,257,536 encoder-arena
bytes; ordinary compact mode consumes 97,936,896. This controls the allocation
extent/layout while varying retained writes. It does not isolate every cache
or instruction-level consequence of those writes.

Across the three studies, all 1,664 payloads match independent historical
encodings. The two reconstruction studies also verify 512 reconstructed frames
and all 1,536 planes against independent entropy decoding. The reservation
study performs no reconstruction. Its producer copied an unused aggregate
`all_reconstructed_planes_exact=true` field; raw frame fields correctly say
null. The independent audit explicitly rejects that aggregate as evidence and
reports no plane verification for the allocation control. Original bytes remain
unchanged; no acceptance criterion or numeric measurement is weakened.

The independent stdlib audit recomputes medians and block order, verifies every
source/harness/driver/binary hash, and confirms the production Pi executable
still has SHA-256 `c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`.
The isolated local prototype source is restored. All measured children finish.
These are unpaced codec diagnostics without color rendering, capture queues,
writing or final fsync. Neither a new camera rate nor a closed deadline gate
follows. Inputs are decoded compressed camera GPRs, not sensor truth.

[Summary](stock-preview-recompute-receipts/summary.json),
[code, protocols, raw events and audit](stock-preview-recompute-receipts/evidence.tar.gz)
and [member hashes](stock-preview-recompute-receipts/manifest.json) preserve
reproduction details. Full artifacts remain under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_preview_recompute_20261031` and the
matching isolated Pi root. Source media and binaries are not redistributed.
