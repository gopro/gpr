# GVID MOV Wrapper

This tool packs opaque FUSED frame payloads into MOV and unpacks them for
downstream decoding. It is a compatibility/export wrapper; `.gvid` remains
the primary raw-video capture container. See
[GVID Conformance](../../docs/GVID_CONFORMANCE.md).

## Build and use

The Makefile requires a C compiler, `pkg-config`, and the FFmpeg
`libavformat`, `libavcodec`, and `libavutil` development libraries.
Run from the repository root:

```bash
make -C tools/gvid_mov
export GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-artifacts}"
mkdir -p "$GPR_ARTIFACT_ROOT"
tools/gvid_mov/gvid_mov_pack --fps 20 --quality 3 --cfa RGGB --bit-depth 14 \
  --black-level "${GPR_BLACK_LEVEL:?Set the source black level}" \
  --white-level "${GPR_WHITE_LEVEL:?Set the source white level}" \
  "$GPR_ARTIFACT_ROOT/frames" "$GPR_ARTIFACT_ROOT/wrapped.mov"
tools/gvid_mov/gvid_mov_unpack "$GPR_ARTIFACT_ROOT/wrapped.mov" "$GPR_ARTIFACT_ROOT/unpacked"
ffprobe -show_streams -show_format "$GPR_ARTIFACT_ROOT/wrapped.mov"
```

Supply compatible FUSED `.gpr` frames under `frames/` in filename order.
Match fps, quality, CFA, bit depth, and black/white levels to the actual
source; these options describe metadata and do not recompress or repair
payloads. Dimensions default to the frame header and can be overridden for
the decoded Bayer output size. Inspect the resulting stream and compare
unpacked payloads before relying on a new FFmpeg build.

## Format and API

The MOV sample tag is `GPRr`, with one intra-frame packet per FUSED payload.
The shared reader also accepts the `GPR1` tag written by the macOS
`tools/gpr2prores/gpr_mov_tool` frontend. Both tags work with the desktop
MOV importer and patched FFmpeg decoder. Raw MOV packets are not
ordinary rendered video and require a compatible decoder.

Metadata fields include codec version, quality, CFA, bit depth,
black/white levels, encoder settings, color matrix, and optional source
traceability. Use portable source identifiers when sharing artifacts.
Container metadata does not broaden the FUSED Bayer-format support.
The API can attach packet metadata, but this is not a supported per-frame
metadata roundtrip through MOV. Use source DNG metadata for desktop rendering;
keep the macOS frontend's external `.exif` sidecar when retaining EXIF.

[include/gvid_mov.h](include/gvid_mov.h) defines writer/reader signatures and
ownership. Check create, add-frame, close, and read return values. Reader
payload memory is valid only until the next read or close; copy it if it
must outlive that call. Requested fps determines MOV timing, independently
of measured encode or reconstruction speed.

Payload roundtrip checks live in the companion
[validation branch](https://github.com/dcliftreaves/gpr/tree/review/product-validation/tools/test).
