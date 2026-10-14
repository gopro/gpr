# Getting Started

This checkpoint contains the shared still codec, metadata support and SDK.
Later feature commits add fused Bayer, video, preview and desktop tools.

## Build and convert a still

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 3
build/source/app/gpr_tools/gpr_tools -i input.dng -o output.gpr
build/source/app/gpr_tools/gpr_tools -i output.gpr -o decoded.dng
```

## Native build options

`GPR_BUILD_SDK` enables the DNG/XMP-backed SDK. `GPR_BUILD_STILL_APPS`
enables `gpr_tools` and requires that SDK. Both default on.
`GPR_BUILD_LEGACY_CLI` independently enables the old encoder/decoder
applications and defaults off. The shared codec libraries are always built.
Disable both SDK and still applications for a shared-codec-only build.

`NEON` defaults on for ARM64. Use `NEON=OFF` for a scalar build.

See [Stills](STILLS.md) for input formats and preservation limits.
