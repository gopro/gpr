# Portable 1x/2x reconstruction on stock VC-5

Legacy BIBO architecture code is bundled with the installed tools. Checkpoint
selection remains explicit; nothing is downloaded or enabled by default.
An optional [repository 1x candidate](stock-cleanup-strength.md) now passes the
declared four-reference numeric guard. Its weights are separate from installed
camera/SDK packages; broader quality approval remains open.
The original source is `tools/cnn/model.py` in PR #95 / upstream #71 at
`8be29eee57b4637865b572f18ab9e3f4edaa2236`. State-dict names and arithmetic are
retained. Supporting its 2x architecture does not make a fused-profile model
appropriate for stock input.

## Use

For the scoped stock-Q3 1x candidate, choose its repository checkpoint explicitly:

```sh
python tools/stock_still.py restore source.gpr --sample-bits 14 \
  --sdk build/source/app/gpr_tools/gpr_tools \
  --checkpoint models/stock-q3-rggb14-cleanup/selected.pt \
  --device mps --output restored.dng
```

An explicit `--scale 2` selects a 2x checkpoint's output contract. A mismatch
between the requested scale and checkpoint is rejected. The 2x skip path is
clipped per-plane bicubic interpolation (`align_corners=False`) plus the saved
residual scale and network output, matching the earlier BIBO runtime formula.
Caller overrides remain available through `--cnn-root`; the older separate
cleanup-checkpoint schema still requires that external architecture directory.

For an experimental 2x sequence from a stock RGGB14 recording:

```sh
python tools/stock_video.py restore recording.gvid \
  --decoder build/source/app/stock_video/stock_video \
  --checkpoint /path/to/explicit-2x-candidate.pt --scale 2 \
  --device mps --editable dng --out-dir reconstructed
```

No tested legacy 2x checkpoint is recommended for stock input; the commands
establish functional support for explicit candidates, not quality approval.
[Still restored GPR encoding](stock-cnn-quality.md#optional-reconstructed-gpr)
now optionally follows 1x/2x reconstruction with an explicit lossy preset.
Recorded-sequence restored GPR encoding and arbitrary-CFA/count-domain
reconstruction remain pending. Non-reconstructed stills retain broader formats.

Torch/numpy are optional reconstruction dependencies. The normal inspector,
encoder, decoder and geometry helpers import without Torch. Both new modules
are included in capture/preview/camera/full packages; weights are not included.

## Geometry and calibration

1x keeps the supported typed calibration exactly. For 2x, the output records
both source and output dimensions/calibration hashes and a geometry-change
accounting. It scales ActiveArea, DefaultCropOrigin/Size, MaskedAreas, EXIF pixel
dimensions and focal-plane pixel resolution. CFA phase, black/white levels,
matrices and white balance retain their count/color meaning. These are source
calibration values, not a newly measured post-CNN noise model.

GainMaps in OpcodeList2 are transformed in the stage-2 active-image domain,
following the bundled Adobe SDK's pixel-center normalization. Bayer-site
origins retain parity: an original odd site remains odd, rather than moving
to an even site when coordinates double. Pitch-one maps split into four
site-specific maps; pitch-two maps retain their selected site. Gain entries,
grid spacing, opcode version and flags are preserved. Normalized grid origins
shift to follow the per-plane interpolation coordinate mapping
`old_i = (new_i + 0.5) / 2 - 0.5`. Thus the same spatial calibration field is
evaluated at corresponding source positions.

Normalized WarpRectilinear/Fisheye/Vignette parameters in OpcodeList3 retain
their bytes after shape/finite-parameter checks. Unknown spatial opcodes,
black-level row/column deltas, unsupported GainMap grids/pitches and nonempty
GainMaps with an odd active-area origin fail explicitly. The current GainMap
adapter accepts single-plane Bayer maps, pitches 1/2 and an even active-area
origin. It does not silently preserve stale coordinates for unsupported cases.

Opaque XMP is omitted for 2x because it may contain conflicting pixel/crop
geometry; the receipt records that omission and reason. EXIF/GPS and other
supported source tags retain the typed packaging boundaries described in
[editable RAW](stock-editable-raw.md). Later rendering/quality work must still
verify display color, image detail/noise and temporal behavior.

## Functional evidence

- CPU inference with the real 1x stock-Q3 and 2x diverse checkpoints matches
  the external architecture **bit for bit** on two input tensor shapes.
  [Architecture receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/architecture-parity.json).
- A full 45.7 MP 1x Nikon output has exactly the same reconstructed pixel and
  DNG hashes as the earlier external-architecture measurement.
  [Receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/bundled-1x.json).
- A supplied 4096x3072 camera GPR produces an **8192x6144** editable DNG with
  transformed GainMaps/crops. Independent SDK RAW decoding matches every
  reconstructed sample, and SDK calibration parsing succeeds.
  [Receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/camera-2x.json). There is no higher-resolution
  camera truth for this sample, so it provides no quality approval.
- Seven optional Torch tests pass locally: known-filter halo seams against
  whole-image inference, finite/shape rejection, declared 2x bicubic tiling,
  bundled zero-residual checkpoint contracts, calibrated 1x/2x still workflows,
  native stock recording-to-2x-editable-DNG sequence integration, and audited
  training/checkpoint/split-isolation contracts.
- Ordinary media tests independently check geometry, fractional crops,
  masked areas, EXIF, all four GainMap sites, pitch-one splitting, preserved
  flags/grid data, analytic gain interpolation and malformed/unsupported cases.
- All ten local Release and ASan/UBSan targets pass. All four relocated package
  profiles pass, including imports without Torch. Their five installed runtime
  modules match the measured implementation. The relocated full package also
  runs the real 1x checkpoint using the bundled architecture from an unrelated
  working directory. [Package receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/packages-mac.json).

Reproduce optional contract tests without trained weights:

```sh
python tests/stock_cnn_test.py --sdk build/source/app/gpr_tools/gpr_tools \
  --capture build/gpr_capture_app --decoder build/source/app/stock_video/stock_video
```

Linux Release CI now includes these CPU/Torch tests. Hosted checks on this
implementation remain a separate gate. Pi Release also passes all ten ordinary CTest targets in 2.79 seconds at
`31274ea`, including the geometry/GainMap checks: [receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/pi5-release.json).
Its 410 source/test/tool/build/fixture files were verified against Git. This
milestone adds no Pi Torch claim.

## 2x quality: legacy transfer rejected

The evaluation starts with four independently decoded lossless Nikon RGGB14
DNGs (8280x5520). Each color plane is downsampled by rounded 2x2 area averaging,
then the resulting 4140x2760 Bayer RAW is actually stock-Q3 encoded and decoded.
All four checkpoint candidates receive those **same decoded low-resolution
counts**. Their reconstructed Bayer is compared against the original source
and a clipped bicubic reconstruction of the identical decoded input.

This deliberately tests a declared simulated stock path. It is not the old
fused decimator, a live sensor sequence, a temporal gate or an inherited model
approval. Low-resolution metadata is explicitly derived from source calibration;
opaque XMP is omitted. The simulation currently rejects source stage-1/2
opcodes and black delta arrays that it cannot safely downsample. Training-split
membership of the old checkpoint data is unknown.

| Legacy 2x checkpoint suffix | 0001 ΔdB | 0067 ΔdB | 5323 ΔdB | 6693 ΔdB | Pooled ΔdB |
|---|---:|---:|---:|---:|---:|
| `ANE` | -6.59 | -3.95 | -0.98 | -1.02 | -2.09 |
| `ML2_q3_dec2` | -9.88 | -6.42 | -0.70 | -0.46 | -3.16 |
| `ML2_q3_dec2_diverse` | -10.11 | -7.22 | -0.88 | -0.64 | -3.37 |
| `HH1x4` | -5.65 | -3.24 | -0.47 | -0.51 | -1.46 |

Every entry is model PSNR minus bicubic PSNR, using full-scale 16383 and the
source active area. The baseline pooled PSNR is **44.44 dB**. Pooling uses
squared error across equal-size/equal-active-area frames before converting to
dB. All **16 comparisons regress**; no candidate is promoted or recommended.
The measurement runtime sources are hash-verified against commit `31274ea`.
All four references use integral ActiveArea coordinates. Per-site,
shadow/highlight and reference-gradient metrics are in the receipts:
[summary](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/summary.json), [0001](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/z8-0001.json),
[0067](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/z8-0067.json), [5323](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/z8-5323.json),
[6693](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-sr-receipts/z8-6693.json). Source pixels and weights are not committed.

```sh
python tools/benchmark_stock_sr.py --source source.dng \
  --sdk build/source/app/gpr_tools/gpr_tools \
  --checkpoint /path/to/candidate.pt --device mps --output-dir /new/sr-benchmark
```

A [fresh stock-input training/selection path](stock-sr-training.md) now uses
separate source groups and a frozen validation-selected checkpoint. Its four
final-image overall gains are modest and shadows regress; it remains unapproved. Functional
2x packaging is implemented; stock-matched reconstruction quality, restored
tiers, rendered color/noise/detail and temporal approval remain unfinished.

## Full-frame GainMap simulation

Same-site downsampling for synthetic SR pairs now shares one metadata helper
across benchmark/training and UPRESABLE analysis workflows. It accepts the
four full-frame pitch-two Bayer GainMaps found in the supplied Mission 1 files,
using sampling-centroid coordinates; other stage-1 operations, cropped/partial
maps and unsupported stage-2 operations remain errors. The simulation does not
model physical sensor binning or exact commutation of varying gains with area
averaging. [Actual GoPro checks](stock-cnn-gopro.md) document exact supported
high-metadata roundtrips, independent geometry tests and scoped model quality.
