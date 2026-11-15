# Bayer layouts around the stock codec

The low-level `vc5_encoder` and `vc5_decoder` C APIs now expose word-packed
12-, 14- and 16-bit counts for RGGB, GBRG, GRBG and BGGR. Existing enum values
stay unchanged; appended public values match the earlier feature PR. Samples
use native-endian unsigned 16-bit words. The caller supplies dimensions,
encoder byte pitch and the correct format; a bare stock payload does not
identify the original Bayer layout or count domain.

This keeps the historical **12-bit logarithmic component transform**, curve,
wavelet and entropy profile. Encoding maps 12-bit samples directly to the
4096-entry curve, 14-bit samples through `sample >> 2` and 16-bit samples
through `sample >> 4`. Thus a 16-bit input container is supported, but this
does not preserve sixteen bits of sensor precision or reproduce the earlier
feature PR's higher-precision codec. Decoding applies the stock inverse curve
and writes its linear 16-bit result, or shifts it right for 12-/14-bit output.
The codec is lossy at every supported quality preset.

The existing RGGB14 assembly/NEON and streaming-unpack path stays selected
exactly as before. Additional layouts use direct scalar component unpacking,
without an intermediate full-frame Bayer permutation. This fallback supports
unaligned input and odd byte pitches using byte-safe sample loads. Other
historical input paths retain their existing alignment requirements. Channel
encoding can still run in parallel after the additional layouts are unpacked;
they do not use RGGB14's streaming-unpack optimization. No equivalent speed
claim is made for those formats.

G1/G2 placement follows the earlier feature PR, with rows shown in sensor order:

| Format | Top row | Bottom row |
|---|---|---|
| RGGB | R G1 | G2 B |
| GBRG | G1 B | R G2 |
| GRBG | G1 R | B G2 |
| BGGR | B G1 | G2 R |

## Verification

```sh
cmake -S . -B build-codec -DCMAKE_BUILD_TYPE=Release \
  -DGPR_BUILD_SDK=OFF -DGPR_BUILD_STILL_APPS=OFF -DGPR_BUILD_LEGACY_CLI=OFF \
  -DGPR_BUILD_CODEC_TESTS=ON
cmake --build build-codec --parallel 4
ctest --test-dir build-codec --output-on-failure
GPR_STREAM_UNPACK=1 GPR_BAND_STREAM=1 GPR_COMPACT_BANDS=1 GPR_DIRECT_BANDS=1 \
  ./build-codec/bayer_formats_test tests/fixtures/stock_preview --large
```

The standard test checks 288 combinations: all twelve word formats, six stock
quality presets, two widths and two inputs (textured and black/saturation/edge
patterns). Low input bits deliberately differ below the stock curve's input
precision. All equivalent inputs must produce the same stock payload, and
every output sample must have the expected phase and linear-count scaling.
The two Q3 textured payloads also match independently generated historical
stock fixtures. Additional layouts use unaligned/padded input; guards and
input pixels must remain unchanged. This proves stock-domain equivalence,
not extra sensor fidelity. `--large` checks all twelve formats at 4096x3072 Q3.

The camera package profile also runs the standard codec test, including the
Windows serial implementation. Linux sanitizer CI runs it in the full build.
CTest runs both the default configuration and a second pass requesting all
streaming/compact/direct-band flags.
Local worker-disabled scalar sanitizer checks exercise all fast-path flags
being requested; separately, parallel checks exercise compact/direct bands.

For implementation `04b93e9`, local macOS Release and ASan/UBSan pass all nine
runtime tests; worker-disabled/no-NEON sanitizer builds pass both codec tests.
Pi 5 passes all nine Release tests, both codec tests under ASan/UBSan with leak
detection, and the 12-format 4096x3072 Q3 check with optimization flags set.
[The Pi receipt](stock-bayer-pi5.json) records logs, binary hashes, build settings
and 93 codec/common/test/build file hashes checked against the local sources.
These are correctness checks, not comparative encoding speed measurements.

## Remaining integration

The [still SDK format port](stock-still-formats.md) extends its public mapping
and DNG CFA/calibration handling with separate validation and explicit schema
limits. The [stock still preset workflow](stock-still-quality.md) now covers
Q0–Q5 selection and typed calibration. Restored-tier/model quality approval
remains pending. Capture/GVID, preview and Python video adapters retain
their documented format restrictions.
Uncompressed editable DNG packaging already accepts all four Bayer phases and
12/14/16-bit domains; that alone does not extend any encoder frontend.
