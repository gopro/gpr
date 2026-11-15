# Supplied 2x checkpoint through a fresh streaming recording

The currently supplied 2x checkpoint now has a complete video/export check on
a new two-frame Pi streaming recording. Both native inputs retain their
4096x3072 geometry and stock-Q3 calibration. This closes the integration gap
between the earlier complete chain, which used a different checkpoint, and
the supplied model's separate native execution checks.

The checkpoint is unchanged:
`f7bda0449b94be4ebd3617297b9dee04506b00e38788dba2d0b04eb71fdaa2f4`.
Its [quality scope](stock-sr-source-regions.md) remains a diagnostic. These
checks establish integration and backend agreement, not recovered-detail,
restored-tier, motion, sensor-truth or sustained recording approval.

## Checked chain

1. The Pi's public streaming capture path records two native inputs with
   calibrated 1024x768 preview. Every recorded payload, source calibration,
   frame/PTS association, delivered preview pixel/CRC and END3 matches the
   existing independent stock/offline references. The capture is retained
   for downstream processing, with a verified transfer hash.
2. Standard-library commands extract exact original GPR payloads and editable
   DNGs. Native hybrid inference with the supplied model produces 8192x6144
   Bayer outputs and correctly scaled calibration, without Torch at runtime.
3. A fresh Torch MPS invocation restores the identical decoded inputs using
   that same checkpoint. Maximum native/Torch difference is **one count** on
   each frame, within the existing folded-FP16 budget of 32. Differing samples
   are 375,682 and 414,898 out of 50,331,648 per frame. The bound is unchanged.
4. Native captured-GPR timelapse produces model-output DNG, explicit-Q5 GPR,
   final-decoded DNG, GVID and private MOV. Both Q5 payloads exactly match an
   independently built historical reference encoder using the actual native
   model counts. That reference includes its documented bounds/sign fixes;
   it is not presented as an unmodified source build.
5. Direct native reconstruction from the original recording produces an
   **8192x6144 ProRes HQ** movie. Both frames decode independently with FFmpeg.
   Native model counts match across editable video restore, timelapse and
   direct ProRes. Input payload/calibration hashes and actual capture-relative
   timestamps remain bound to their original frames.

The ProRes file is 45,400,115 bytes, with BT.709 tags and `yuv422p10le` output.
Its TIFF intermediate uses 16 bits per component; HQ is not described as a
16-bit final codec. Actual relative packet timestamps are 0 and 52,769,887 ns,
with a final nominal duration of 50,000,000 ns. Timestamp error is zero at the
recorded nanosecond timescale. The separate timelapse explicitly uses synthetic
nominal timestamps 0 and 50,000,000 ns; those are not substituted for capture
PTS in the direct movie.

All ten editable DNGs are independently read with tifffile. Their count
hashes match their respective model or final-codec outputs, and supported
calibration matches the scale-two transform. Opaque XMP is omitted with the
existing stale-geometry accounting. Re-encoded GPR counts remain distinct from
pre-encode CNN counts. Original and reconstructed private MOV round-trips
recover their complete respective GVID bytes exactly.

## Native ProRes settings correction

The audit found `cnn_settings: null` when standalone native reconstruction was
selected: receipt creation checked for a Python model object. It now checks
for reconstruction model information, recording tile, halo, host-device option
and scale for both Python and native paths. Native backend/artifact identities
remain in the separate model record. This changes receipt metadata only.

The real chain checks the actual settings `256/64/scale 2`. The existing native
contract test also asserts `16/8/scale 2` for its standard-library ProRes case.
Fresh local native contracts pass all 18 primary scale/backend/shape rows,
scalar/vector/worker comparisons, all four Core ML unit policies and the
stdlib-only still, video, timelapse and ProRes workflows. Broader sanitizer
work is not added.

## Reproduction and evidence

The [audit](stock-selected-chain-receipts/audit.json) independently checks
TIFF pixels, supported calibration, backend count differences, payloads,
container round-trips and ProRes timing/format. The
[manifest](stock-selected-chain-receipts/manifest.json) binds the compressed
code, predeclared commands/protocols, source/binary/model hashes, numeric logs,
reference encoder snapshots and native-contract receipt. No media or weights
are included in this evidence archive.

Native media commands run under `python -S`. Optional Torch comparison and
independent image analysis use a separate environment. Its initial OpenMP
runtime abort occurred before comparator output; NumPy-first loading resolves
it without enabling duplicate OpenMP runtimes. A first wrapper omitted the
script import path and stopped before output; its correction, original errors
and continuation bindings are retained. Successful capture/native stages are
reused with exact hashes rather than relabeled as reruns. Core ML test-only
dependencies are installed in a temporary target directory, leaving the
existing environment unchanged.

Full artifacts remain local under
`/Volumes/OWC_8TB/gpr_work/artifacts/stock_selected_video_chain_20261031`.
The directly playable two-frame sample is `direct-native-2x-prores.mov`;
`upres/` contains editable model/final files and exact private interchange.
For commands, start with [native runtime](stock-native-runtime.md) and
[UPRESABLE](stock-upres-workflow.md), then use the archived driver to reproduce
this exact chain with fresh output paths and the hash-bound inputs/artifact.
This is a two-frame integration example, not a temporal-quality demonstration.

## Fresh-recording Pi CPU follow-up

Both newly recorded native inputs now also pass the supplied checkpoint's Pi
CPU path, with four Torch threads, tile 256 and halo 64. Pi Torch 2.14.1 CPU
outputs differ from the matching Mac Torch 2.12 MPS reference by at most
**one count**, preserving the preregistered one-count portability bound.
Only 196 and 223 of 50,331,648 samples differ per frame. Pi output also differs
from the native Mac hybrid output by at most one count in these two cases.
The native backend's separate 32-count budget is not substituted for the
stricter CPU/MPS comparison.

Pi inference takes **97.996 and 98.949 seconds per frame**; the two-frame
command takes 200.923 seconds including model setup, decoding and publication.
These are offline costs, not camera recording or sustained inference claims.
Independent TIFF reads on the Pi confirm both DNGs' count hashes and supported
scale-two calibration. Source payload, original PTS, decoded input and model
hashes match the Mac comparison exactly. Earlier Pi evidence used different
prepared inputs; it is not relabeled as this fresh native-input run.

The [comparison](stock-selected-chain-receipts/pi-comparison.json) and
[Pi DNG audit](stock-selected-chain-receipts/pi-dng-audit.json), plus their
predeclared protocol, actual driver and output identities in the archive,
retain the result. Source revision 589bc29 passes all seven hosted jobs in
[run 41188343975](https://github.com/dcliftreaves/gpr/actions/runs/41188343975).
This closes the fresh-input execution/count check; strict recording deadlines,
model-quality scope and physical camera limitations remain unchanged.
