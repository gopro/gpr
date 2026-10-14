# gpr2prores

macOS GPR/DNG renderer with optional CNN restoration, Metal or Core Image
demosaic, and ProRes 422 HQ output.

## Build

Requires macOS 14+, Xcode with the Metal compiler, CMake, Clang, pkg-config,
LibRaw, and FFmpeg development libraries (libavformat, libavcodec, libavutil,
libswresample). From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
make -C tools/gpr2prores
```

The default `CNN_BACKENDS=none` build decodes, demosaics, writes ProRes, and
packs/unpacks MOV without CNN objects, CoreML, MPS/MPSGraph, or CNN kernels.
Metal remains required for demosaic/downscale. Select optional backends explicitly:

```sh
make -C tools/gpr2prores CNN_BACKENDS=coreml
make -C tools/gpr2prores CNN_BACKENDS=metal  # hybrid Metal and pure MPSGraph
make -C tools/gpr2prores CNN_BACKENDS=both
make -C tools/gpr2prores CNN_BACKENDS=none  # return to the default
```

Only selected backend objects, frameworks, and kernels are compiled/linked.
CoreML uses CPU postprocessing and needs no CNN Metal kernels. A build
configuration stamp invalidates objects and metallibs when options change;
switching selections does not require `make clean`. Builds share output paths,
so do not run different selections concurrently in the same directory. Pass the
same `CNN_BACKENDS` selection to `make install` (omitting it selects `none`).

Backend build/selection tests live in the companion
[validation branch](https://github.com/dcliftreaves/gpr/tree/review/product-validation/tools/test).
`test_desktop_backend_config.py` checks build selection with mock tools;
`test_desktop_build_profiles.sh` explicitly builds each backend on macOS.

Override `GPR_BUILD=/path/to/build` for an external CMake build. Source headers
always come from this repository. Set `PKG_CONFIG_PATH` for dependency prefixes,
or override `LIBRAW_CFLAGS`, `LIBRAW_LIBS`, `FFMPEG_CFLAGS`, and `FFMPEG_LIBS`.
Keep `default.metallib` and `Demosaic.metallib` beside the executable.
`make install PREFIX=...` installs both tools and links their Metal libraries.

## Render

```sh
tools/gpr2prores/gpr2prores --meta-dng sample.dng \
  --demosaic core-image --out-resolution uhd clip.gvid out.mov

# Optional restoration after building with CNN_BACKENDS=metal or both:
tools/gpr2prores/gpr2prores --meta-dng sample.dng \
  --ckpt models/weights_1x --cnn-backend metal --cnn-scale 1x \
  --demosaic core-image --out-resolution uhd clip.gvid out.mov
```

Inputs: a single `.gpr` or `.dng`, a directory of either (sorted by filename),
`.gvid`, or a GPRr/GPR1-tagged `.mov`. Containers unpack to temporary
frame directories. GPR/container inputs require source DNG metadata through
`--meta-dng` unless a companion DNG is discovered. DNG input provides its own
metadata and performs an encode/decode round trip unless `--no-codec` is given.

| Option | Values |
|---|---|
| `--cnn-backend` | `coreml`, `mpsgraph`, `metal`; must be compiled in |
| `--ckpt` | CoreML model package, or fp16 weight directory for Metal/MPSGraph |
| `--cnn-scale` | `2x` (default) or `1x`; must match the model |
| `--no-cnn` | Decode, demosaic, and encode without a model |
| `--demosaic` | `metal-bilinear` (default) or `core-image` |
| `--out-resolution` | `2k`, `uhd`, `4k`, `6k`, `8k` (default, native dimensions) |
| `--fps` | Output rate, default 24 |
| `--max-frames`, `--timing`, `--skip-errors` | Frame limit, stage timings, or continue on frame errors |
| `--gvid-dispatch` | Validate a `gvid_runtime_dispatch.v1` plan; per-tile model invocation is not implemented |

See `--help` for all options. The retained experimental `--mission-look` mode
requires `--demosaic core-image`; its CPU tone pass can be slow.

With `CNN_BACKENDS=none`, `--no-cnn` is implicit: output is decoded/demosaiced,
not CNN-restored. Explicit `--ckpt` or `--cnn-backend` options are errors, even
with `--no-cnn`; model environment variables do not enable restoration.
CNN-enabled builds run restoration by default, using `coreml` when available
and otherwise `metal`. Use `--no-cnn` to bypass it. Requesting an unavailable
backend is always an error, never a fallback. `--help` reports compiled backend
availability and the default. The existing Metal backend's internal execution
fallbacks are unchanged.

## Models and Configuration

For CNN-enabled rendering, checkpoint precedence: `--ckpt`, then
`GPR_SUPER_RES_MLPACKAGE`, then
`super_res.mlpackage` under `GPR_MODEL_ROOT`. Without `GPR_MODEL_ROOT`, the model
directory is `$GPR_EXTERNAL_ROOT/models` when configured, otherwise `models`
under the current working directory. For Metal/MPSGraph, supply a compatible
weight directory with `--ckpt` instead of the default CoreML package path.

Exporters require Python 3, NumPy, and PyTorch:

```sh
python3 tools/gpr2prores/extract_F_weights.py --ckpt checkpoints/F.pt --out models/F_weights
python3 tools/gpr2prores/extract_F_ane_weights.py --ckpt checkpoints/F_ane.pt --out models/F_ane_weights
```

Use matching architecture and scale. The first exporter supports both the
Metal and MPSGraph loaders; ANE weights include folded batch normalization.

`TMPDIR` selects temporary storage. Diagnostic settings include
`SUPERRES_PROFILE=1`, `SUPERRES_NOFUSE_POST=1`, and
`CNN_COREML_UNITS=cpu|gpu|ane|all` (default `all`).

## MOV Companion

```sh
tools/gpr2prores/gpr_mov_tool pack frames clip.mov --fps 24 \
  --tc-start 01:00:00:00 --meta-dir dngs --audio audio.wav
tools/gpr2prores/gpr_mov_tool info clip.mov
tools/gpr2prores/gpr_mov_tool unpack clip.mov unpacked --prefix frame
```

Pack/unpack preserves GPR payloads. Both MOV frontends share the portable
reader and accept `GPRr` and `GPR1` sample tags. The macOS writer adds optional
audio/timecode and a sibling `.exif` file; the portable
[GVID MOV wrapper](../gvid_mov/README.md) offers rational frame rates and
track metadata. Keep MOV wrappers named `.mov`, not `.gvid`: the capture
stream and MOV have different framing.

Unpacking exports encoded frames, not audio or a reconstructed DNG metadata
set. Rendering still requires `--meta-dng`; it does not silently recover
white balance from the MOV's EXIF sidecar. For patched FFmpeg decoding, see
[gvid_codec](../gvid_codec/README.md).

## Adoption

This desktop layer needs the shared and fused codecs plus GVID framing, but
not the capture API or native preview. Model weights and CNN backends are
optional; the default build needs neither. Training and quality-gated model
selection are separate from this executable.

Desktop output and kernel checks live in the companion
[validation branch](https://github.com/dcliftreaves/gpr/tree/review/product-validation).

## Archived Research

One-off validation, corpus preparation, post-training scripts, and the
superseded `Makefile.container` remain available at their original paths on
`archive/research-and-integration-2026-09-09` at `3d675ef`.
The native `test_F_ane_kernels.m` harness is in the validation branch.
