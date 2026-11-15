# Independent stock RAW-media builds and packages

The camera capture and preview libraries build without the DNG/XMP still SDK.
Encoder and decoder selection is independent. Historical desktop defaults
remain enabled: SDK, gpr_tools, both codecs and the legacy VC-5 applications.
Capture, preview and experimental tools remain opt-in.

| Profile | SDK | Encoder | Decoder | Public package targets |
|---|---|---|---|---|
| Capture | Off | On | Off | `GPR::capture`, `GPR::encoder`, `GPR::gvid` |
| Preview | Off | Off | On | `GPR::preview`, `GPR::decoder`, `GPR::gvid` |
| Camera | Off | On | On | Capture and preview targets together |
| Desktop SDK | On | On | On | `GPR::sdk`, encoder and decoder |
| Full media | On | On | On | SDK, capture and preview together |

For a camera library build:

```sh
cmake -S . -B build/camera -DCMAKE_BUILD_TYPE=Release \
  -DGPR_BUILD_SDK=OFF -DGPR_BUILD_STILL_APPS=OFF -DGPR_BUILD_LEGACY_CLI=OFF \
  -DGPR_ENABLE_VIDEO=ON -DGPR_ENABLE_PREVIEW=ON -DGPR_BUILD_PACKAGE=ON
cmake --build build/camera --parallel 4
cmake --install build/camera --prefix /tmp/gpr-camera
cpack --config build/camera/CPackConfig.cmake -G ZIP
```

For capture-only, additionally set `GPR_BUILD_DECODER=OFF` and leave preview
disabled. For preview-only, set `GPR_BUILD_ENCODER=OFF`, leave video disabled
and enable `GPR_ENABLE_PREVIEW`. A conflicting selection fails configuration.
Disabling the SDK requires disabling still applications too. Disabling legacy
applications avoids their parser and MD5 dependencies.

`GPR_BUILD_CAPTURE_APP` and `GPR_BUILD_PREVIEW_TOOLS` add the POSIX replay and
preview frontends. Enabling preview tools also enables the preview library.
The library-only switch is `GPR_ENABLE_PREVIEW`; no frontend is required.
`GPR_BUILD_STOCK_VIDEO_EXPERIMENT` adds the POSIX stock-video encoder/decoder
frontend. Linux and macOS full packages include these tools when selected.
Windows native packages contain the libraries; those POSIX frontends remain
unavailable on Windows. Windows channel encoding uses the serial fallback.
Compact high-pass band allocation requires active memory-stream channel
workers. Serial and file-stream encoding use full bands, even when callers
request the compact/streaming optimization flags. Linux sanitizer CI exercises
the serial configuration with those flags set.

## Installed C and C++ clients

The package installs static libraries, self-contained public headers, CMake
targets, stock Python inspection/editable-RAW tools, documentation and selected
dependency notices. `GPR::sdk` supplies its static dependency closure; clients
do not need to enumerate DNG, XMP, Expat, MD5 or JPEG libraries. SDK capability
definitions reflect the selected codecs. Shared codec utilities are exposed as
`GPR::common` and `GPR::codec_common`.

```cmake
cmake_minimum_required(VERSION 3.5)
project(my_camera C CXX)
find_package(GPR CONFIG REQUIRED COMPONENTS capture preview)
add_executable(my_camera camera.cpp)
target_link_libraries(my_camera PRIVATE GPR::capture GPR::preview)
```

Configure the client with `-DCMAKE_PREFIX_PATH=/tmp/gpr-camera`, then include
`gpr_capture.h` and `gpr_preview.h`. The APIs accept C callers; capture/reader/
preview implementation requires the C++ runtime, so enable C++ in the client
project even when application sources are C. Required unavailable components
are rejected by `find_package`. Choose the combined camera package when both
capture and preview are needed, rather than loading two packages into the same
CMake namespace.

Native media packages require zlib and a platform thread runtime. Pure codec
and SDK builds do not introduce a zlib dependency. POSIX builds explicitly
propagate the math/thread link dependencies. Package consumers find these
system dependencies on their own machine rather than inheriting producer paths.

Archives carry profile, OS and CPU names. Public headers live in `include/gpr`,
libraries in `lib`, programs in `bin`, and scripts/docs/notices in `share/gpr`.
`gpr-build-info.txt` records source revision hints, optional source-snapshot
digest, compiler versions, components and optimization settings. Package
version 0.1.0 is experimental and separate from the historical SDK version.
The package check records source snapshots and archive hashes in its receipt.

Static archives are toolchain-specific. Default release IPO can require the
producer compiler's LTO implementation; use the matching compiler or rebuild
with `GPR_ENABLE_IPO=OFF` when distributing across compiler families. This is
not a universal binary or a camera firmware image. CNN architecture/weights
and optional Python inference dependencies are not bundled.

After extracting a full package, the installed tools can be used directly:

```sh
python3 /prefix/share/gpr/tools/stock_video.py inspect clip.gvid
python3 /prefix/share/gpr/tools/raw_media.py metadata source.GPR \
  --sample-bits 14 --output calibration.json
```

See [capture ownership](stock-capture-api.md), [preview conventions](stock-preview.md)
and [editable exports](stock-editable-raw.md) for the runtime and calibration
contracts. Successful packaging does not add support for further Bayer encoding
formats, calibrated display processing, camera firmware deployment or approved
CNN quality.

## Verification and reproducibility

```sh
python3 tests/build_profiles.py --work-dir /tmp/new-profile-check \
  --full-build build-runtime
```

The optional full build must contain SDK, both codecs, capture, preview and
the selected POSIX tools. The checker reconfigures/builds it before packaging.
It also creates three source trees with the SDK dependencies physically absent
and the unused codec absent from capture-only/preview-only. Each is built and
tested, then packaged. The producer source, build and install directories are
deleted before an outside C/C++ client finds the extracted ZIP.

Clients encode/capture the independent stock fixture, read and finalize the
recording, decode Bayer and match independent preview RGB. A separate SDK client
links only `GPR::sdk` and wraps/extracts/decodes VC-5 data. The full archive also
runs its installed Python tools and native frontends through editable DNG/GPR
export and checks decoded pixel identity. The Python ZIP test restores stored
POSIX executable modes, as normal native archive tools do.

The release CI jobs create downloadable ZIPs and validation logs. The Windows
job uses vcpkg zlib and validates native capture/preview packages with MSVC.
Platform results must be taken from the corresponding completed CI/hardware
receipts; configuration alone is not evidence of platform support.

For implementation `14c8dce`, [Windows native packaging and Linux Release and
sanitizer checks](https://github.com/dcliftreaves/gpr/actions/runs/40723187365)
passed. Windows checked all three stripped native profiles, deleted their
producer trees and ran external C/C++ clients from relocated archives. The
macOS job for that revision was still queued when this note was written;
earlier macOS Release packaging and the committed local macOS/Pi receipts
provide separate evidence for their recorded source revisions. Windows checks
do not cover the desktop DNG SDK or POSIX applications.
