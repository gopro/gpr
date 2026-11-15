# Supplied GoPro samples: current CNNs, calibration and rendered fidelity

The supplied stock-Q3 checkpoints now have direct camera-file evidence beyond
the Nikon studies. Both 1x cases and the 2x case improve all four RAW regions,
preserve supported editable calibration, and improve matched 16-bit rendered
RGB/luma fidelity. The checkpoints and their research status are unchanged.

## Frozen models on the supplied files

| Input | Workflow | RAW active gain | Rendered RGB gain | Rendered Y-prime gain |
|---|---|---:|---:|---:|
| GP011728.GPR | 12 MP stock Q3 + 1x cleanup | +0.497856 dB | +0.481091 dB | +0.364923 dB |
| GPAB1094.GPR | 12 MP stock Q3 + 1x cleanup | +0.548516 dB | +0.200485 dB | +0.381503 dB |
| GP011689.GPR | 50 MP reference → simulated 12 MP stock Q3 → 2x | +0.082827 dB | +0.284302 dB | +0.370891 dB |

These are fixed-model evaluations, without model/strength selection on these
results. Source pixels are **SDK-decoded camera GPR counts**, which already
contain the camera's compression. They are fidelity references for additional
compression/reconstruction, not lossless sensor truth or a noise-free target.
The 2x case uses same-site area averaging and actual stock Q3, not physical
sensor binning or a native synchronized 12/50 MP capture pair.

The source GPR hashes, frozen checkpoint hashes, encoded payload identities,
count references and output hashes are bound before evaluation. Independent
TIFF reads of editable source DNGs agree with a separate direct SDK decode of
the original GPRs. Reconstructed DNG counts and supported calibration are
checked independently. All twelve RAW regional comparisons pass.

The [completed shared-helper receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/shared-summary.json)
and [protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/shared-protocol.json) retain exact
metrics, changed sample counts, source/model identities and metadata checks.
A fresh shared-helper run reproduces the initial one-off camera results;
no new checkpoint replaces either supplied model.

## Full-frame GainMaps in the simulation helper

The original synthetic downsampling helper rejected every nonempty stage-2
opcode list, including the four GainMaps in these actual Mission 1 files.
The common `stock_cnn_metadata.simulated_half` helper now supports four distinct
full-active-frame, pitch-two, single-plane Bayer GainMaps. The benchmark helper
uses that same implementation, eliminating duplicated geometry logic.

For site s, low sample `(2*j+s)` averages high samples `(4*j+s)` and
`(4*j+s+2)`. With normalized pixel centers, low map origin is shifted by
`(s-0.5)/high_dimension`; normalized spacing and grid values are retained.
The bundled [Adobe SDK interpolator](../source/lib/dng_sdk/dng_gain_map.cpp)
uses the half-pixel center convention. Independent analytic ramp tests cover
all four phases and nonzero origins. Rejection cases cover partial/cropped
maps, duplicate phases, unsupported opcodes, malformed lists and invalid gains.

For these three actual camera metadata packets, downsampling followed by the
existing 2x transform reproduces **all supported high-resolution calibration
byte exactly**, including all four GainMaps. Opaque XMP retains the existing
explicit omission policy. This is a synthetic sampling-centroid transform:
applying a spatially varying gain and averaging do not generally commute.
It does not establish exact radiometric equivalence to a camera's native binning.
Other stage-1/2 operations and unsupported map geometries remain rejected.

The [actual-packet proof](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/gainmap-proof.json), shared
camera rerun and Pi metadata checks retain that boundary. The original
simulator failure and its initial partial two-case result remain archived.

## Matched calibrated rendering

Reference, stock and CNN counts are each packed with the identical supported
metadata, then rendered through the existing `gpr_render`/`CIRAWFilter` path.
The 2x reference render uses the same supported high metadata as the restored
file, including the explicit XMP omission, to isolate pixel changes. All nine
renders have matching orientation, white-balance settings, baseline exposure,
lens-correction setting and RAW decoder within each case.

Rendering uses exposure zero and disables adjustable enhancement; it writes
16-bit integer RGBA Rec.709 TIFFs. Metrics exclude alpha and compare encoded
RGB/Y-prime values, not scene-linear radiance. All three RGB and luma MSEs
improve. The first NumPy matrix-multiply analysis emitted floating-point
warnings despite finite results. An independent chunked, explicit-channel
audit with floating errors set to raise confirms every metric within the fixed
1e-10 relative agreement check; those audited numbers are reported above.
[Render records](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/render-summary.json),
[audit protocol](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/render-audit-protocol.json) and
[independent audit](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/render-audit.json) bind the result.

Local review files are under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_cnn_supplied_gopro_shared_20261030/render-review/`.
Each source subdirectory contains 16-bit TIFFs, per-render receipts, individual
previews and `comparison.png` (reference/stock/CNN, overview plus a
reference-selected high-variation crop). These display PNGs quantize to eight
bits and retain the Rec.709 ICC profile; full-precision masters remain external.

Visual inspection shows subtle changes. The low-light references themselves
are noisy, and their high-variation crops select noise as well as structure;
these images do not demonstrate removal of original sensor noise. The 2x
result remains softer than the 50 MP reference. Useful recovered texture,
broader appearance, temporal consistency and restored-tier approval remain
open despite the positive scoped fidelity measurements.

## Focused Mac/Pi verification

The Mac raw-media suite (11 tests), CNN suite (9), sequence suite (1) and
UPRESABLE workflow suite (2) pass. The same 23 test methods pass against an
isolated Pi source snapshot, and all three actual metadata packets roundtrip
exactly on Pi. The production Pi capture executable remains
`c576f5662ac605cc5719ae536308e9ff71cdcec4f4e714343a5a919a5711687c`.
No camera codec, queue, model weights or system scheduling setting changes.

The Pi setup initially assumed the MOV bridge was in the capture build; it is
in the existing non-sanitized `build-mov` directory. The first isolated suite
then lacked the checked-in preview fixture: CNN/workflow/sequence passed,
while raw-media reported fixture-read failures. Those receipts/logs are retained.
After copying and hashing the original fixtures, raw-media is rerun and passes;
the three passing suites are reused only after proving their source and binary
hashes unchanged. The final receipt identifies that reuse rather than claiming
one fresh combined invocation.

[Mac/Pi verification](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/qa-receipt.json),
[Pi receipt](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/pi-receipt.json) and
[receipt manifest](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-cnn-gopro-receipts/manifest.json) retain logs and hashes.
The prior full hosted revision `2680e70` passes all seven jobs; this source
change requires its own hosted validation. Physical sensor/display/SD/thermal
qualification is unchanged.
