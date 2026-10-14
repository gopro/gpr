# Getting Started

Run commands from the repository root. The library and command-line still
tools use CMake and a C/C++ toolchain. Python 3 is needed for container and
reconstruction tools. Desktop ProRes review uses macOS, Apple Silicon, Xcode
command-line tools, and the dependencies named in the playback Makefile.

## Build and convert a still

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
export GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-artifacts}"
mkdir -p "$GPR_ARTIFACT_ROOT"
build/source/app/gpr_tools/gpr_tools -i data/sample.dng -o "$GPR_ARTIFACT_ROOT/sample.gpr" -q 3
build/source/app/gpr_tools/gpr_tools -i "$GPR_ARTIFACT_ROOT/sample.gpr" -o "$GPR_ARTIFACT_ROOT/decoded.dng"
```

Supply your own `data/sample.dng`; that filename is an example, not a bundled
fixture. These commands exercise the codec without a CNN. Approved
CNN-assisted still tiers require the matched weights described in
[Reconstruction](RECONSTRUCTION.md).

## Native build options

The default POSIX build produces `gpr_tools`, `fused_decode_cli`, and
`gvid_preview_rgb_cli`, plus the still SDK and capture libraries. It does not
build benchmarks, coefficient diagnostics, legacy codec applications, or tests.
Use separate build directories for different deployments:

| CMake option | Default | Included when enabled |
|---|---|---|
| `GPR_BUILD_SDK` | ON | DNG/XMP-backed GPR still SDK |
| `GPR_BUILD_STILL_APPS` | ON | `gpr_tools`; requires SDK |
| `GPR_ENABLE_VIDEO` | ON on POSIX | Capture queue and labs encoder API |
| `GPR_BUILD_PREVIEW_TOOLS` | ON on POSIX | `gvid_preview_rgb_cli`, `fused_decode_cli` |
| `GPR_BUILD_CODEC_UTILITIES` | OFF | `coeff_io_tool`; required by UPRESABLE workflows |
| `GPR_BUILD_LEGACY_CLI` | OFF | `vc5_encoder_app`, `vc5_decoder_app` |
| `GPR_BUILD_DIAGNOSTICS` | OFF | `bench_fused`, `labs_encoder_bench_cli`; requires video |

Camera-only capture and playback, without DNG/XMP or desktop dependencies:

```bash
cmake -S . -B build-camera -DCMAKE_BUILD_TYPE=Release \
  -DGPR_BUILD_SDK=OFF -DGPR_BUILD_STILL_APPS=OFF
cmake --build build-camera --parallel 4
```

For stills only, set `-DGPR_ENABLE_VIDEO=OFF -DGPR_BUILD_PREVIEW_TOOLS=OFF`.
For SDK libraries without still applications, set `-DGPR_BUILD_STILL_APPS=OFF`.
Windows defaults capture and preview off; those POSIX components cannot be
enabled there. Disabled video sources are omitted from the library, not merely
hidden from command-line help. The companion validation branch additionally
requires `-DBUILD_TESTING=ON` to build tests; runtime builds do not import it.

Linux ARM64 defaults to `FUSED_TUNE_CPU=native`. Use
`-DFUSED_TUNE_CPU=cortex-a76` for Pi 5 or `-DFUSED_TUNE_CPU=` for a
generic build. NEON defaults on for ARM64; `-DNEON=OFF` disables the
option-controlled paths, not every compiler-selected entropy kernel.

`FUSED_LOG_POLYNOMIAL`, `FUSED_TIMING`, `FUSED_TIMING_DETAIL`, and
`JANS_INLINE_PROFILE_RUNTIME` are opt-in and default off. Polynomial log
requires NEON; detailed timing enables stage timing. These are CMake
options, separate from runtime codec settings. See
[CMakeLists.txt](../CMakeLists.txt) and [Architecture](architecture.md).

## Pack raw-video frames

Place compatible per-frame FUSED `.gpr` payloads in
`$GPR_ARTIFACT_ROOT/frames`, in filename order. Match every header option to
the actual encoded frames; packing does not resize or recompress them.

```bash
python3 tools/gvid_pack.py "$GPR_ARTIFACT_ROOT/frames" "$GPR_ARTIFACT_ROOT/clip.gvid" \
  --width 4096 --height 3072 --fps 20 --quality 8 --pixel-format 1
```

For source metadata, additionally pass
`--metadata "$GPR_ARTIFACT_ROOT/clip.gvid.meta.json"` with a valid
`gvid_source_metadata.v1` sidecar. See [GVID Conformance](GVID_CONFORMANCE.md)
for validation, metadata requirements, and recovery semantics.

Pi/camera capture is a separate integration workflow; see
[Capture API](CAPTURE_API.md) and [Native Preview](NATIVE_PREVIEW.md).
See [Product Details](PRODUCT_DETAILS.md) for the buffered-write performance
boundary and outstanding camera validation.

## Desktop review without a model

Complete the Release build above first. The playback Makefile links the
libraries under `build/` by default.

```bash
make -C tools/gpr2prores
tools/gpr2prores/gpr2prores \
  --meta-dng data/sample.dng --no-cnn \
  --demosaic core-image --out-resolution uhd \
  "$GPR_ARTIFACT_ROOT/clip.gvid" "$GPR_ARTIFACT_ROOT/review.mov"
```

The metadata DNG must match the clip's camera and color interpretation.
This is a codec-only rendered review, not CNN reconstruction. MOV/GPR1 is
an optional compatibility/export wrapper; `.gvid` is the primary raw-video
container.

For model-backed 4K cleanup or 8K SR, follow
[Reconstruction](RECONSTRUCTION.md) for exact model approval scope. Both run offline on
desktop hardware. Weight files and large source fixtures are not included in
this checkout, and the build alone does not reproduce their quality receipts.

## Verification

See [Testing Methodology](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/TESTING_METHODOLOGY.md)
on the companion `review/product-validation` branch for validation procedures.
