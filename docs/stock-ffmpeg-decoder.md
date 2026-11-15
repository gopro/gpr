# Optional stock decoder in FFmpeg

`tools/ffmpeg_stock` adds `gpr_stock` to an explicitly chosen FFmpeg n8.0
checkout, pinned to commit `140fd653aed8cad774f991ba083e2d01e86420c7`.
It handles the private GVS2/GVS3 MOV profiles through the stock decoder and
outputs 16-bit little-endian RGGB/GBRG/GRBG/BGGR Bayer with source 12/14/16-bit
counts. It does not rescale counts, apply camera calibration or demosaic RAW.
FFmpeg PNG/ProRes conversion is a convenience path, distinct from the calibrated
macOS RAW/ProRes renderer. Models are not invoked by this decoder.

The adapter registers a separate codec ID and tags from CFHD and fused
GPRr/GPR1. It is an out-of-tree integration, not an upstream FFmpeg decoder or
a plugin that changes applications using their own bundled FFmpeg.

## Build and verify

Linux/macOS require a compiler, CMake, make, git, Python, pkg-config and zlib.
The stock MOV bridge additionally needs system libavformat/libavcodec/libavutil
development packages; x86 FFmpeg configure needs nasm. The verifier uses NumPy
and tifffile. Use a fresh source checkout: the installer modifies codec
registration files and copies the adapter into the supplied FFmpeg tree.

```sh
cmake -S . -B build-ffmpeg -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DGPR_ENABLE_IPO=OFF \
  -DGPR_BUILD_SDK=OFF -DGPR_BUILD_STILL_APPS=OFF -DGPR_BUILD_LEGACY_CLI=OFF \
  -DGPR_ENABLE_VIDEO=ON -DGPR_BUILD_CAPTURE_APP=ON -DGPR_BUILD_PREVIEW_TOOLS=ON \
  -DGPR_BUILD_STOCK_VIDEO_EXPERIMENT=ON -DGPR_BUILD_MOV_TOOLS=ON -DGPR_BUILD_PACKAGE=ON
cmake --build build-ffmpeg --parallel 4
cmake --install build-ffmpeg --prefix /absolute/path/gpr-stock-prefix
git clone --depth 1 --branch n8.0 https://github.com/FFmpeg/FFmpeg.git /absolute/path/ffmpeg-stock
python3 tools/ffmpeg_stock/build.py build --ffmpeg-source /absolute/path/ffmpeg-stock \
  --gpr-prefix /absolute/path/gpr-stock-prefix --jobs 4
python3 tests/stock_ffmpeg_test.py --ffmpeg /absolute/path/ffmpeg-stock/ffmpeg \
  --ffprobe /absolute/path/ffmpeg-stock/ffprobe --bridge build-ffmpeg/gpr_mov \
  --capture build-ffmpeg/gpr_capture_app --decoder build-ffmpeg/source/app/stock_video/stock_video \
  --receipt new-stock-ffmpeg-receipt.json
```

Jobs are bounded to 1..8. The installer checks the pinned commit and expected
registration anchors before edits, and is idempotent. `install` instead of
`build` applies only the source changes. No FFmpeg sources or binary weights
are bundled in this repository. Ordinary library builds acquire no FFmpeg
decoder dependency. The CI job `FFmpegStockDecoder` builds and runs the same
integration independently of the main native test jobs.

```sh
/absolute/path/ffmpeg-stock/ffmpeg -i recording.mov -map 0:v:0 \
  -fps_mode passthrough -c:v rawvideo -f rawvideo sensor.raw
```

## Framing and compatibility

New stock MOV files include the original checked GVID clip prefix in a standard
`glbl` sample-description atom. It carries CFA/count-domain information to the
decoder independently of MOV metadata. Original frame records, clip metadata,
absolute timestamps and complete GVID recovery remain unchanged. Old stock MOV
files without this atom still unwrap with the bridge; unwrap then rewrap them
to add decoder configuration. The optional decoder rejects missing or invalid
configuration rather than guessing CFA or source depth.

Initialization validates the prefix using the native GVID reader and checks
MOV dimensions/tag against it. Each sample must contain one complete FRM2/FRM3
record. V3 header, metadata and payload CRCs are checked before decoding, as are
VC-5 profile/framing/geometry; payloads are limited to 128 MiB. Compressed bytes
are copied to aligned scratch. Output dimensions and byte count must match the
clip. Packet seeking may change record indices, so decode does not require a
monotonic record sequence. Complete recording/END3 verification remains the
GVID/MOV transport reader's responsibility, not a per-packet decoder claim.

The integration verifier encodes fresh 128x96 sources for all twelve CFA/depth
combinations, compares two decoded frames each against the direct stock
decoder, checks both GVS2/GVS3 against a frozen independent VC-5 fixture,
rejects corrupted clip CRC/record CRC/payload and exercises PNG/ProRes output.
This proves functional Bayer identity, not camera color or temporal quality.

The matrix passes on [Mac](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-ffmpeg-receipts/mac.json) and
[Pi 5/Linux](https://github.com/dcliftreaves/gpr/blob/170802d44c28cde77dfdb9c31bb45244297c21a8/docs/stock-ffmpeg-receipts/pi5.json), with identical decoded pixel hashes
for all twelve formats. Both receipts bind the decoder and helper binaries by
SHA-256. Ordinary Mac Release passes 23/23 tests, including MOV/ProRes; the
Pi's 13 transport/A/V tests also pass with the new sample-description atom.
Four relocated package profiles pass, and the installed adapter/installer pair
was invoked successfully. Hosted FFmpeg x86 validation is a separate CI job.
