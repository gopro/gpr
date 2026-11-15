# Prepared, calibrated native preview

The native preview library now has a separate calibrated display API alongside
the historical byte-compatible SDK RGB API. It uses the decoder's reduced
16-bit linear camera RGB output, removes the legacy horizontal mirror, applies
source black/white calibration and supported stage-two Bayer GainMaps, clips
at the source camera white point, converts to linear sRGB, applies baseline
exposure, encodes sRGB and applies source crop/orientation. It does not build a
full-resolution Bayer image, invoke a CNN or link the DNG SDK into the camera
preview library.

This is a **wavelet preview approximation**, not the full-resolution RAW
renderer. Green difference is not reconstructed: the reduced green signal is
used for both green sites, corrected separately and averaged. Wavelet filtering
and inverse logarithmic conversion do not commute with full RAW demosaicing or
spatial correction. GainMaps are sampled at reduced sensor-grid centers. Color
look tables, custom tone curves and highlight reconstruction are outside this
matrix display path. No rendered or temporal quality approval is implied.

## Prepare and play a recording

The optional host compiler uses the **vendored DNG SDK's color specification**
for AsShotNeutral, dual illuminants, analog/individual/reference calibration,
forward matrices and chromatic adaptation. It parses metadata only. Each unique
typed calibration is wrapped with an existing stock payload as a temporary
GPR; no compression or image decoding occurs in profile preparation.

```sh
cmake -S . -B build-camera -DCMAKE_BUILD_TYPE=Release \
  -DGPR_ENABLE_VIDEO=ON -DGPR_BUILD_CAPTURE_APP=ON \
  -DGPR_BUILD_PREVIEW_TOOLS=ON -DGPR_BUILD_PREVIEW_TESTS=ON
cmake --build build-camera --target gpr_preview_app gpr_preview_profile --parallel 4

python3 tools/stock_preview.py recording.gvid --compiler build-camera/gpr_preview_profile \
  --downsample 8 --output-dir prepared-profiles
build-camera/gpr_preview_app --input recording.gvid --profile-dir prepared-profiles --runs 5
```

`gpr_preview_profile` is built with POSIX still applications and needs the full
DNG/XMP host dependencies. The resulting parameters are portable: capture-only,
preview-only and combined camera libraries still build without that SDK. A
camera caller can supply the equivalent typed calibration directly. No Python,
SDK or JSON parser is required inside `gpr_preview`.

Preparation selects per-frame `raw_metadata`, then clip `raw_metadata`, then an
explicit `--metadata-file` default. It validates the complete recording before
publishing the new profile directory, rechecks source identity, protects existing
paths and records source/compiler/profile/metadata/payload hashes. Publication
uses the still tools' exclusive link/copy fallback and rollback behavior; it is
not group-atomic or an fsync guarantee. The new directory must not already exist.
Files are limited to 100,000 frames and 16 MiB per native profile.

`--profile-dir` reads the prepared `000000.gprp`, `000001.gprp`, etc. Each has a
small `.gprp.bind` companion covering frame index, original uint64 timestamp,
payload size/CRC, exact frame/clip metadata CRCs and the selected profile CRC.
The player rejects stale, swapped, missing or corrupted bindings. These are
integrity/association checks, not cryptographic authentication. The JSON receipt
also contains SHA-256 identities. The player continues validating the complete
recording when `--limit` restricts rendering.

Prepared profiles carry their reduction; `--downsample` is therefore rejected
alongside `--profile-dir`. `--output-dir new_directory` writes oriented sRGB PPMs.
Without `--profile-dir`, the player retains its historical gains, gamma and
mirror convention and still matches the stock RGB fixtures exactly.

## Native API and precision

`gpr_preview_calibration_identity` initializes a synthetic identity transform,
not a camera default. Real callers supply the source parameters:

- Four black values in original sensor Bayer-site order and a scalar WhiteLevel
  in the source 12/14-bit count domain. The preparer aligns black repeats to
  ActiveArea, including odd active origins.
- Camera-to-linear-sRGB matrix, normalized camera-white clipping values and a
  linear baseline-exposure multiplier. The white-point clipping follows the
  DNG SDK baseline RGB transform and avoids treating saturated sensor counts as
  an arbitrary illuminant.
- Absolute active/crop rectangles and EXIF orientation 1..8. Fractional default
  crop coordinates are sampled on the reduced grid; orientation is applied
  after crop and color processing.
- Up to 32 single-plane GainMaps with pitch one/two, Bayer-site masks and
  bilinear grids up to 256x256. Per-site products and saturation limits preserve
  the ordering of overlapping gain/attenuation operations. Green-site black,
  gain and saturation corrections precede green averaging.

`gpr_preview_plan_create` copies/prepares the required sampling and gain data.
Caller calibration buffers may expire afterward. The immutable plan can be
rendered concurrently with separate output buffers; `plan_info` gives oriented
dimensions, and `plan_destroy` releases it. The serialized equivalent uses
`gpr_preview_plan_from_profile`. RGB24 output respects arbitrary caller pitch
and capacity; row padding and canaries are tested. Unaligned compressed inputs
are copied to aligned scratch. New plans require sensor dimensions at least 48.

Plans precompute spatial gains once. With maps, storage is eight float factors
per reduced, unrotated sample plus row/column indices; without maps there is no
gain table. At 1024x768 this is about 25.2 MB per mapped plan. The player keeps a
bounded four-plan LRU cache, shared across playback passes. Real camera callers
control plan lifetime and scheduling.

The [bounded asynchronous queue](stock-capture-preview.md) can retain a plan
and copied compressed payload for one active/one pending frame. It adds explicit
latest-pending replacement and sink error/drain contracts. Pi combined capture
measurements demonstrate its CPU cost; physical display integration and reliable
live 30 fps remain unverified.

Reduced stock RAW preview decodes the independent GS/RG/BG VC-5 channels in
parallel by skipping non-selected channel payloads and allocating only the
selected transform. The AArch64 fast path consumes these host-endian reduced
wavelet planes directly: it reconstructs RGB, applies the decoder log curve,
and continues through calibration without materializing the historical
six-byte-per-pixel intermediate RGB image. `GPR_PREVIEW_SERIAL_DECODER=1`
retains the ordinary decoder path as a bit-exact comparison mode.
`gpr_preview_plan_render_wavelets` exposes the same renderer for producers that
already have reconstructed reduced GS/RG/BG planes. It validates exact reduced
geometry, natural sample alignment, per-plane capacity and RGB output capacity
before writing output. Plan calibration and source count-domain conversion are
identical to compressed preview. Plane pointers remain caller-owned and are
borrowed only for the synchronous call. This interface avoids decompression
when a producer already has the required samples; capture currently still uses
the compressed-payload queue. Encoder intermediate coefficients must first be
reconstructed to the decoder's 14-bit wavelet sample domain; directly passing
quantized or prescaled encoder bands is not supported.

The fast encoder cannot supply these planes by borrowing its final transform:
`AllocEncoderTransforms` in `source/lib/vc5_encoder/encoder.c` retains each LL
plane but allocates only one row per highpass band when compact bands are
enabled. `TransformVertical` overwrites those rows after incremental entropy
coding. The decoder's quarter-scale LL is instead reconstructed from the two
upper wavelet levels by `ReconstructWaveletBand` in
`source/lib/vc5_decoder/decoder.c`, including inverse companding, quantization
and prescale handling. The existing encoder thumbnail only consumes the
sixteenth-scale LL and does not establish quarter-scale equivalence.
An encoder-side producer therefore needs to retain the upper-level highpass
coefficients before reuse (or reconstruct them incrementally), reproduce their
encoded companding, and perform those two inverse levels. Its acceptance gate
is exact reduced planes/display pixels against independently decoded stock
payloads at every quality, while preserving payload bytes and measuring total
capture latency, memory and recording deadlines on Pi 5. That producer is not
implemented by the public plane-rendering entry point.

An internal architecture proof now implements this retention/reconstruction in
`tests/encoder_preview_probe.c`. Its optional private encoder observer retains
the two upper highpass levels, while the largest level stays compact. After
encoding, the proof reproduces the codebook's cubic companding and performs two
decoder inverse transforms. At all six qualities, the resulting GS/RG/BG planes
match ordinary compressed decoding exactly and the payload matches the encoder
without observation byte for byte. Mac and Pi tests exercise both ordinary and
compact/direct-band modes. On Pi, both existing real 4096x3072 RGGB14 replay
sources (`GP017517.raw`, `GP017348.raw`, identified by hashes in the Mission 1
receipts) also pass at all six qualities with compact/direct-band flags.

Reproduce the focused small-fixture tests with
`ctest --test-dir build-runtime --output-on-failure -R '^encoder_preview_probe'`.
For a tightly packed 4096x3072 RGGB14 source, run
`GPR_STREAM_UNPACK=1 GPR_BAND_STREAM=1 GPR_COMPACT_BANDS=1 GPR_DIRECT_BANDS=1 GPR_CHANNEL_ASM=1 GPR_PIN_CHANNELS=1 build-runtime/encoder_preview_probe source.raw`.
The proof's observer is internal; the public encoder ABI and capture pipeline
remain unchanged. It allocates reconstruction scratch and retains upper bands
for all four channels. A capture producer still needs bounded reusable scratch,
failure/ownership handling and a measured combined replay before this can be
claimed as a preview latency improvement. No timing gate is closed by this
functional proof.

The retained coefficients are now also available through the additive
`vc5_encoder_process_with_preview` API. Existing parameter structures and the
ordinary process function preserve their ABI. The synchronous callback receives
an in-process `vc5_preview_coefficients` view containing typed GS/RG/BG upper
levels with geometry, strides, capacities, quantization and prescale. Its
pointers expire when the callback returns. The callback runs under the existing
global encoder lock and must not reenter encoder or SDK encoding functions.

`vc5_decoder_reconstruct_preview` converts this view into three caller-owned
quarter-scale planes suitable for `gpr_preview_plan_render_wavelets`. It checks
geometry, capacity, alignment, quantization and prescale before reconstruction;
failed reconstruction releases partial outputs. The encoder remains independent
of the decoder library. The public callback/reconstruction path is compared
with both the internal retained-band proof and compressed decoding at all six
qualities, including both real Pi replay sources. Its shared header and symbols
are included in relocated packages. These measurements still cover RGGB14;
broader source-domain calibration parity requires additional fixtures.

The helper currently allocates intermediate wavelets per invocation. Capture can
now select this callback through `--preview-from-encoder`, reconstruct channels
concurrently and feed copied planes into its bounded display queue. The
[capture integration receipts](stock-capture-preview.md#capture-frontend) measure
the whole path; typical Pi latency improves to 46.07 ms p95. Sustained tail
latency and storage flush behavior remain open. Capture now supplies reusable
per-channel scratch arenas to the helper; the public helper still honors the
caller's chosen allocator. [Scratch ownership and measurements](stock-capture-preview.md#capture-frontend).
The preview contract compares serial and fused output byte for byte over 192
cases spanning both fixture widths, all three reductions, all eight
orientations, RGGB/GBRG calibration and 12/14-bit count domains, with crop,
GainMap, unaligned-input, pitched-output and canary coverage. Both ordinary
NEON and forced-scalar test invocations pass on Mac and Pi 5.
The same matrix also compares explicitly decoded planes through the public
wavelet entry point with compressed rendering, and checks short-plane,
wrong-geometry and short-output rejection without modifying output canaries.

On AArch64 with NEON enabled, a four-pixel kernel loads the three wavelet
planes and processes independent pixels in vector registers through RGB
reconstruction, black normalization, gains, clipping and the 3x3 transform.
Non-contiguous crop samples and tails use the scalar path. `GPR_PREVIEW_SCALAR=1`
forces scalar processing for comparison. Both paths use a shared 65,536-entry
sRGB lookup; independent floating-point references allow one RGB8 count of
rounding difference. Legacy RGB8 behavior is unchanged.

The preparer rejects unsupported linearization tables, row/column black deltas,
non-unit DefaultScale, non-full DefaultUserCrop and nonempty stage-one/three
opcodes. Supported source GainMaps are retained; profile hue/saturation/look and
tone-curve tags are explicitly reported as omitted looks. Full lens/rendering
workflows remain available through [editable RAW](stock-editable-raw.md) and the
separate [macOS RAW/ProRes renderer](stock-prores.md).

## Evidence and Pi measurements

Six new linear 16-bit big-endian fixtures come from a fresh build of the
historical stock revision `6b22975dcf4e65ab9445852cd7b2220ae3faa3af`.
The original six RGB8 fixtures also match that fresh build. The calibrated C++
contract exercises **192 combinations** of two geometries, three reductions,
eight orientations, two CFA layouts and two count domains, with crop, fractional
and distinct Bayer-site blacks, gains, sequential saturation, source-white
clipping, unaligned payloads and padded RGB rows. It also checks immutable-plan
concurrency, caller map lifetime, malformed profiles, every profile truncation
and byte corruption, capacity failure and nonfinite parameters.

Five stdlib integration tests cover the DNG color compiler, neutral-white and
dual-illuminant response, per-frame WB/orientation changes, profile cache reuse,
odd ActiveArea origins, typed GainMaps, unsupported metadata, profile/frame
binding, strict finalization, protected outputs and non-regular inputs.

Pi 5 results use the same **12-frame 4096x3072 stock-Q3 still-derived replay** as
[MOV validation](stock-mov.md). Each mode renders five passes / 60 frames, with
two prepared camera calibrations. Warm USB-SSD playback includes source reading,
CRC/end checks, frame binding checks, profile reads/preparation, RGB checksums
and per-frame JSON logging; no PPM writes are timed.

| Calibrated output | Decode + display color median | Full wall throughput |
|---|---:|---:|
| 1024x768, NEON | 66.27 ms | 13.77 previews/s |
| 1024x768, forced scalar, same binary | 89.92 ms | 10.49 previews/s |
| 512x384, NEON | 16.28 ms | 50.77 previews/s |
| 256x192, NEON | 3.18 ms | 173.26 previews/s |

The NEON kernel reduces the 1024x768 decode/color median by about 26% (1.36x).
Two 12 MP source profiles and preview pixels also matched across Mac/Pi in the
checked runs. These are standalone playback measurements, **not simultaneous
capture, sensor delivery, display presentation or live camera frame rates**.
Combined capture/preview scheduling and actual display integration remain work
in the active rebuild. No CNN runs in this path.

[Pi source/binary identities and every benchmark event](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-calibrated-preview-receipts/pi5.json),
[local Release/ASan/UBSan/TSan and stock-reference checks](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-calibrated-preview-receipts/validation.json),
and [relocated-package evidence](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-calibrated-preview-receipts/packages.json)
record this milestone. Pi passes all four preview targets with ASan/UBSan/leak
checks in 145.60 s; local full Release passes all fourteen targets. The Pi
snapshot verifies 430 source/test/tool/build/fixture files against Git `269f814`.
