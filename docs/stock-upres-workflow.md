# Stock UPRESABLE workflow

`stock_upres.py` recreates the applicable timelapse, regression and combined
orchestration from the original feature branch using the stock encoder.
It requires an explicit 2x checkpoint and an explicit preset for each new encode. It does
not select, download or approve weights. Python NumPy, tifffile and Torch are
optional workflow dependencies; native capture does not depend on them.

For each ordered input, the workflow checks the source calibration and count
domain, independently checks lossless DNG pixels against SDK decoding, averages
2x2 samples at each Bayer site to simulate half-resolution capture, encodes and
decodes that capture through stock VC-5, runs the selected 2x CNN, and encodes a
full-resolution stock GPR. The model loads once per invocation. A camera GPR
replay can instead be declared explicitly; it is not sensor ground truth.

```sh
python tools/stock_upres.py --mode both \
  --input frame001.dng --input frame002.dng \
  --reference held-out.dng --reference-kind lossless_sensor_dng \
  --checkpoint selected-2x.pt --sdk build-runtime/source/app/gpr_tools/gpr_tools \
  --capture-quality 3 --output-quality 5 --bridge build-mov/gpr_mov \
  --output-dir new-run --dng-export
```

`timelapse` requires ordered `--input` paths and the native MOV bridge;
`regression` requires `--reference` paths. `both` runs both lists. Outputs are
placed in a new directory, with per-frame stock encoding/decoding receipts and
a `run.json` manifest. A failed run retains completed work, records the error,
and keeps `complete` false. Existing output directories are refused.

For existing camera GPR captures, `--input-mode captured_gpr` bypasses the
simulation and its extra encode. Each original VC-5 payload is retained in
`capture.gpr`, decoded through the count-domain carrier, and fed directly to
the 2x model. Its source quality preset is unknown and recorded as null.
`--capture-quality` is unnecessary for this timelapse path. Supported GainMaps
are transformed by the existing 2x calibration path, with opaque XMP omissions
accounted for; no lens metadata is silently removed to permit simulation.

```sh
python tools/stock_upres.py --mode timelapse --input-mode captured_gpr \
  --input GP011728.GPR --input GPAB1094.GPR \
  --reference-kind camera_gpr_replay --checkpoint selected-2x.pt \
  --sdk build-runtime/source/app/gpr_tools/gpr_tools --output-quality 3 \
  --bridge build-mov/gpr_mov --output-dir new-capture-run --dng-export
```

A 4096x3072 captured input produces an 8192x6144 output. There is no independent
50 MP sensor reference for such input; re-encoding loss can be measured, but
reconstruction fidelity cannot be established by comparing it to itself.
Regression references in `both` mode still use the declared simulation flow.

The primary timelapse outputs are `timelapse.gvid` and `timelapse.mov`.
`stock_sequence.py` copies each full-resolution GPR payload without another
encode, preserving per-frame typed calibration. A matching stock encoding
receipt is required for each input to bind the advertised preset, count domain,
metadata and payload. GVID framing, CRCs and END3 are verified before publication;
the MOV wrapper independently verifies exact GVID recovery. Timestamps are
explicitly synthetic, at the requested `--fps-num`/`--fps-den` rate.

`--dng-export` keeps both `model-output.dng` (exact CNN counts) and
`decoded-full.dng` (decoded final GPR counts). These can differ because stock
GPR re-encoding is lossy. Regression reports compare bicubic, model output and
final GPR output against the same reference, including active-area, detail,
shadow, highlight and per-site errors. Re-encoding loss is reported separately.
Every report retains `quality_approval: false`; passing a workflow test does
not approve a model or a visual size/quality tier.

Optional calibrated deep ProRes uses the existing macOS renderer and encoder:

```sh
# Add to a timelapse/both invocation:
--prores --prores-decoder build-runtime/source/app/stock_video/stock_video \
  --prores-renderer build-runtime/gpr_render \
  --prores-encoder build-runtime/gpr_prores --prores-probe /path/to/ffprobe
```

That export renders the final GPR-derived frames without a second CNN pass.
It remains a distinct calibrated rendering path rather than a Bayer master.

Current scope requires RGGB14 and at least 96x96. Simulated source dimensions
must be divisible by four. Simulation requires an empty stage-1 opcode list. Stage 2 may contain the
four checked full-frame, pitch-two Bayer GainMaps; other opcode combinations,
black-level delta arrays and unrepresentable spatial calibration are rejected.
Opaque XMP is omitted with accounting. Timelapse additionally requires full dimensions divisible by eight
and at most 8192x6144. The original 8280-wide Nikon still references can be
evaluated in regression mode but cannot be silently cropped into this video
profile. Standalone native Core ML/MPSGraph/hybrid inference is available for captured-GPR
timelapse through the [native workflow](stock-native-runtime.md). Native
simulation/regression analysis uses the existing analysis workflow. Physical
sensor capture, temporal model approval and automatic checkpoint selection
remain outside this delivery.

Validation: `tests/stock_sequence_test.py` checks exact payloads/calibration,
rational timestamps, native MOV round-trip and mismatched receipt/geometry
rejection. `tests/stock_upres_test.py` exercises all three modes with a
deterministic zero-residual 2x checkpoint, verifies bicubic equivalence, source
identity, DNG calibration, final payload identity, non-overwrite and failed-run
accounting. Mac additionally exercises calibrated ProRes. Revision-bound
receipts are in `stock-upres-receipts/`; these are synthetic functional tests,
not real-scene quality or 12 MP throughput evidence.

`tools/benchmark_stock_upres.py` exercises the captured-GPR path on caller
sources, measures whole-workflow time and checks every final payload against
an independent stock encoder. It also compares decoded final DNG pixels with
the standalone decoder and verifies scaled metadata, original capture payloads,
GVID records and END3. Its receipt binds the checkpoint, binaries and scripts.
The baseline encoder must support the documented `GPR_BENCH_QUALITY` switch.

```sh
python tools/benchmark_stock_upres.py --input GP011728.GPR --input GPAB1094.GPR \
  --checkpoint selected-2x.pt --sdk /path/to/gpr_tools --bridge /path/to/gpr_mov \
  --decoder /path/to/stock_video --reference-encoder /path/to/vc5_encoder_app \
  --threads 4 --quality 3 --output-dir new-benchmark
```

## Real captured-GPR Pi 5 check

[The measured receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-upres-receipts/pi5-real-captured.json) uses the
user-provided `GP011728.GPR` and `GPAB1094.GPR`, both 4096x3072 RGGB14 camera
stills. The explicit checkpoint is the previously evaluated fresh stock 2x
candidate; its held-out shadow regression still prevents model promotion.

Both outputs are 8192x6144. Every final VC-5 payload matches independent stock
encoding, and standalone decoding matches every final editable-DNG pixel.
Original capture payloads, scaled calibration including GainMaps, packed GVID
records, END3 and exact MOV recovery all pass. The two-source offline workflow
takes **114.24 s total / 57.12 s per frame** with four Torch CPU threads;
CNN inference takes **49.95 and 50.17 s**. Whole-process peak RSS, including
the later independent verification, is **1.585 GB**. The timed workflow starts
after the benchmark's Torch import and thread setup; it includes model loading
and requested local file exports. Independent codec checks are timed separately.

Q3 re-encoding introduces active-area RMSE of **11.72 and 5.72 counts** relative
to the exact CNN counts (62.91/69.13 dB in the 14-bit domain). Those numbers
measure encoding loss, not reconstructed detail fidelity. These are two camera
still replays, not motion footage, a live sensor test, real-time CNN evidence
or a quality approval. The new path keeps CNN work offline; it does not change
the existing recording/preview timing measurements.

The [supplied-model video-chain check](stock-selected-video-chain.md) now starts
with a fresh public streaming recording and verifies the currently supplied
2x checkpoint through editable RAW, Q5 re-encoding, exact GVID/MOV and direct
8K ProRes. Its native model counts agree across those paths. The direct movie
retains actual capture-relative timing; this timelapse keeps its explicitly
synthetic clock. No broader model or sustained-rate approval follows.
