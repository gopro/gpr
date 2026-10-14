# GPR FFmpeg Codec

Adds a fused VC-5 decoder to a local FFmpeg build, with `GPRr` and `GPR1` MOV
tags. Output is 16-bit RGGB or GBRG Bayer. Applications must use this patched
FFmpeg build to recognize the codec.

## Build

Requires uninstrumented GPR static libraries built with PIC (Debug or Release), FFmpeg n8.0
git sources, a C compiler, make, Python 3, pkg-config, and zlib development
headers. SDL2 is optional for ffplay. On x86 install nasm.
On macOS, install Xcode command-line tools; satisfy any architecture-specific
assembler requirements reported by FFmpeg configure.

From the GPR repository root:

```sh
export GPR_ROOT="$PWD"
export GPR_BUILD="$GPR_ROOT/build"
export FF_ROOT="$GPR_ROOT/external/ffmpeg_gpr"
cmake -S "$GPR_ROOT" -B "$GPR_BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DGPR_BUILD_DIAGNOSTICS=ON
cmake --build "$GPR_BUILD" --parallel 4 --target bench_fused fused_decode_cli
git clone --depth=1 --branch=n8.0 https://github.com/FFmpeg/FFmpeg.git "$FF_ROOT"
"$GPR_ROOT/tools/gvid_codec/install_patch.sh" "$FF_ROOT"
cd "$FF_ROOT"
"$GPR_ROOT/tools/gvid_codec/configure_and_build.sh"
./ffmpeg -hide_banner -decoders
```

`GPR_BUILD` accepts any compatible CMake build directory; headers come from
`GPR_ROOT/source`. Prefer absolute paths when switching directories.
`PREFIX` defaults to the FFmpeg working directory's `install-gpr`; the script
builds in place and does not install. Set `PKG_CONFIG_PATH` for dependency
prefixes. ffplay is built when SDL2 is detected. `JOBS` defaults to the detected
CPU count, capped at 8; an explicit `JOBS` is also capped at 8. Both scripts
reject a git checkout whose HEAD is not tagged `n8.0`. Missing decoder
registration is fatal at configure time and after the build.

`install_patch.sh` uses its first argument, then `FF_ROOT`, then
`$GPR_EXTERNAL_ROOT/external/ffmpeg_gpr`. If unset, `GPR_EXTERNAL_ROOT` defaults
to `gpr_work` under `RUNNER_TEMP`, `TMPDIR`, or the system temporary directory.
Set paths explicitly for persistent builds.

## Use

After the static FFmpeg build, run on Linux or macOS:

```sh
"$FF_ROOT/ffmpeg" -i input.mov -pix_fmt rgb24 -frames:v 100 frame_%04d.png
"$FF_ROOT/ffmpeg" -i input.mov -vf 'scale,format=rgb48le,scale,format=yuv422p10le' -c:v prores_ks -profile:v 3 out.mov
"$FF_ROOT/ffmpeg" -i input.mov -c:v rawvideo -pix_fmt bayer_rggb16le -f rawvideo out.raw
# When SDL2 was available at build time:
"$FF_ROOT/ffplay" -vf scale,format=rgb24 input.mov
```

Use `bayer_gbrg16le` for GBRG streams. Bayer sample values are not rescaled:
they match `fused_decode_cli`, with `bits_per_raw_sample` taken from FUSD
`log_bits`. PNG/ProRes are convenience conversions, not a camera color pipeline.

The decoder accepts FUSD v1 single-level with LL (16 bands), two-level
(28 bands), and three-level (40 bands, including legacy depth 1). Only
decimation value 2 halves dimensions, matching the direct decoder. Header
and band spans are validated before allocation; packet dimensions override
MOV dimensions. Output dimensions must be multiples of four, at least 16,
and sensor dimensions no greater than 16384. Midstream size changes fail.
GRBG/BGGR pixel formats are not
wired. This is a local integration, not an upstream codec or a plugin for
applications with bundled FFmpeg.

`install_patch.sh` installs `gpr.c` and updates FFmpeg's codec registration.
Use that installer rather than applying historical registration patches.

## Scratch CI Test

The companion validation worktree provides `tools/test/test_ffmpeg_integration.sh`.
It clones n8.0 from the official FFmpeg repository, builds it against existing
GPR libraries, and uses the already-built real `tools/gvid_mov/gvid_mov_pack`
wrapper. Build that wrapper against system FFmpeg development libraries
(`libavformat`, `libavcodec`, `libavutil`, discoverable with pkg-config).
`--mov-pack` can select its location. No third MOV writer is used.

```sh
# From the companion validation checkout:
bash tools/test/test_ffmpeg_integration.sh \
  --runtime-root "$GPR_ROOT" --build "$GPR_BUILD" \
  --scratch-parent "${RUNNER_TEMP:-/tmp}" --jobs 3 --build-timeout 1800
```

Rebuild existing non-PIC GPR libraries with the CMake flag above, especially
on Linux PIE toolchains. Debug or Release libraries work; use uninstrumented
libraries for this job (coverage-instrumented libraries need extra link flags).
The test defaults to three FFmpeg build jobs. It generates small fresh Bayer fixtures through
`bench_fused` GVID, extracts payloads, packs MOV, and compares exact decoded
dimensions, frame counts, CFA, bit depth and bytes with `fused_decode_cli`.
It also checks bad packets, mismatched MOV dimensions, both tags, PNG and
ProRes. Each command has a timeout; the FFmpeg build timeout is capped at
3600 seconds. Failed runs retain scratch artifacts; `--keep` retains successful
runs too. It can run standalone and is included in the companion validation
PR's LinuxDebug CI job.
