# Architecture

## Data paths

| Workflow | Processing |
|---|---|
| Stills | DNG/raw input -> legacy VC5 encoder -> DNG/GPR container -> VC5 decode -> editable Bayer |
| Raw-video capture | Unpacked Bayer -> FUSED encoder -> per-frame payloads -> `.gvid` writer |
| Camera-back preview | Same native 4K `.gvid` -> decode/downsample -> full-frame 1024 x 768 RGB -> display integration |
| Desktop reconstruction | Decode Bayer -> matched optional CNN -> editable raw packaging or demosaic/render -> ProRes |

The camera encodes raw frames and prepares preview. CNN restoration and
super-resolution run on the desktop, independently of the camera capture
budget. A container's declared playback fps is not encoder or CNN throughput.

## Interfaces

| Responsibility | Source |
|---|---|
| Public still conversion API | `source/lib/gpr_sdk/public/gpr.h` |
| Legacy encoder | `source/lib/vc5_encoder/vc5_encoder.h` |
| FUSED payload definition | `source/lib/vc5_encoder/fused_encode.h` |
| FUSED decoder | `source/lib/vc5_decoder/fused_decode.h` |
| Video queue and rate control | `source/lib/vc5_encoder/gpr_video.h` |
| Firmware-facing shim | `source/lib/vc5_encoder/gpr_labs_encoder.h` |
| Raw-video container | `source/lib/vc5_encoder/gpr_video_format.h` |
| Desktop review/export | `tools/gpr2prores/` |

The FUSED pipeline applies the log curve to Bayer samples before forming
the four GS/RG/BG/GD channels, then performs wavelet decomposition,
quantization, and entropy coding. The wrapper records the profile and basic
decode parameters, but not all effective quantization or tuning settings.
Legacy VC5 has a different
bitstream and DNG integration. Consult [SPEC](SPEC.md) and
[Format Specification v2](format-spec-v2.md) before changing either format.

The fused layer adds reusable encoder contexts, decoder paths, ARM kernels,
and coefficient I/O on top of the shared codec. It does not require GVID,
capture, preview, or CNN models. GVID wraps FUSED frames; the capture shim
adds asynchronous submission and writer callbacks. Conventional editable
TIFF/GPR stills use the stills SDK, not the FUSED wrapper. Native build
options are in [Getting Started](GETTING_STARTED.md#native-build-options).

## Integration constraints

Use the public headers as the API authority rather than copying old example
structures. Firmware must supply declared input layout, bounded buffer
ownership, storage error handling, and capture metadata. See
[Capture API](CAPTURE_API.md).

Models must match the codec, preprocessing, Bayer phase, and scale used by
their evidence. Packaging and rendering cannot repair incompatible model
selection. [Reconstruction](RECONSTRUCTION.md) records availability and
promotion requirements.

Historical architecture comparisons and tuning experiments are in the
[archive](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96),
pinned at [`f13dbe9`](https://github.com/dcliftreaves/gpr/tree/f13dbe95d3e97c400e8d493ba178dbb037ff3c96).
