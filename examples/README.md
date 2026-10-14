# API Examples

These examples use the public codec APIs and build separately from the main
CMake targets.

| Example | Purpose |
|---|---|
| [encode_video.c](encode_video.c) | Submit Bayer frames, write a `.gvid` stream through a callback, and report encoder statistics |
| [decode_dng.cpp](decode_dng.cpp) | Convert a GPR still to DNG through the SDK |

## Build

Run from the repository root. The build script currently expects static
libraries under `build/` and invokes Clang/Clang++. This matches the general
quickstart; the example script's library directory is not configurable.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
export GPR_ARTIFACT_ROOT="${GPR_ARTIFACT_ROOT:-artifacts}"
mkdir -p "$GPR_ARTIFACT_ROOT/examples"
OUT_DIR="$GPR_ARTIFACT_ROOT/examples" bash examples/build_examples.sh
```

[build_examples.sh](build_examples.sh) compiles with warnings treated as errors.
The commands explicitly choose a repository-relative output directory.

## Run

Provide your own `data/sample.raw` and `data/sample.gpr` inputs:

```bash
"$GPR_ARTIFACT_ROOT/examples/encode_video" data/sample.raw "$GPR_ARTIFACT_ROOT/clip.gvid"
"$GPR_ARTIFACT_ROOT/examples/decode_dng" data/sample.gpr "$GPR_ARTIFACT_ROOT/decoded.dng"
```

The video example hardcodes one 8280 x 5520 RGGB16 input frame and replays it
for a short clip. It demonstrates API use, not the current Mission 1 capture
profile or measured camera throughput. Change the source constants and input
together when adapting it. A writer callback error exercises the abort path.

For the current native-4K/1024-preview target and firmware ownership contract,
see [Ship Decision](https://github.com/dcliftreaves/gpr/blob/review/product-validation/docs/SHIP_DECISION.md) and
[Capture API](../docs/CAPTURE_API.md).
[Getting Started](../docs/GETTING_STARTED.md) covers ordinary still conversion
and desktop review; [GVID Conformance](../docs/GVID_CONFORMANCE.md) covers
container validation.
