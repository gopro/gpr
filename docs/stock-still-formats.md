# Stock still-image formats and calibration

The still SDK and `gpr_tools` now map RGGB, GBRG, GRBG and BGGR with
word-packed 12/14/16-bit counts through the stock encoder/decoder. Historical
RGGB12P/GBRG12P inputs also remain supported. Public enum values 0–4 and
structure layouts stay unchanged; appended values match the earlier feature
PR and the low-level encoder API.

This uses the [stock logarithmic transform](stock-bayer-formats.md), including
its input quantization. A 16-bit container does not add sixteen-bit internal
precision or lossless compression. The existing RGGB14 assembly path remains
available; additional layouts do not inherit its streaming-unpack speed claim.

## Conversion

```sh
# Native-endian uint16 words, with automatic row pitch from width and format.
gpr_tools -i sensor.raw -o still.gpr -w 4096 -h 3072 -x bggr16
gpr_tools -i sensor.raw -o plain.dng -w 4096 -h 3072 -x bggr16

# An existing calibrated DNG supplies its dimensions, CFA, count domain and
# supported metadata. Uncompressed output uses the decoded Bayer samples.
gpr_tools -i source.dng -o still.gpr
gpr_tools -i still.gpr -o editable.dng
gpr_tools -i still.gpr -o decoded.raw
```

The raw CLI derives WhiteLevel from the selected input format, independently
of the output extension. Its default byte pitch now follows the image width
and packing. `-p` supplies an explicit padded pitch; it must describe the file's
actual rows. Raw input has default calibration unless a parameter file is
supplied. Select the source's real count domain rather than the storage word
size. The old fixed 8,000-byte pitch and output-extension-dependent saturation
selection could produce incorrect small-image/12-bit results.

The SDK's direct `gpr_convert_vc5_to_gpr` API wraps the original stock payload;
`gpr_convert_vc5_to_dng` now produces **uncompressed Bayer DNG**. The latter
previously selected compressed output merely because its input was VC-5.
Decoded words use their actual tight pitch even when the supplied source
parameters described padded or packed raw input. The wrapper checks decoded
dimensions against supplied dimensions. `gpr_convert_dng_to_vc5` now encodes
the source Bayer data with the parsed format and default stock Q3; the
inherited implementation returned an empty buffer without encoding it.

## Metadata behavior

Supported still conversions preserve CFA phase, integer black levels at their
physical sites, full-domain WhiteLevel, orientation, camera neutral/white
balance, source ColorMatrix1/2 and associated illuminants, the supported noise
profile and four equal-sized GainMap records. The public black fields denote
red, green in the red row, green in the blue row, and blue; metadata reads and
writes apply the inverse spatial mapping for each Bayer layout. A 1x1, 1x2 or
2x1 black-level repeat is expanded correctly to the physical 2x2 tile.

Source color matrices retain their numerical scale instead of passing through
the DNG SDK's rendering normalization. An absent second matrix stays absent;
computed default camera profiles retain their previous normalization behavior.
The legacy TIFF writer serializes matrices at 1/10000 precision and black
levels as equivalent rationals, so this is not tag-byte preservation. GainMap
record order, spatial areas, version, flags and data are preserved; the old
reader rewrote their version/flags. Legacy SDK RGB rendering still has the
documented mirror/color-processing limitations; carrying orientation metadata
does not apply that orientation to its RGB output.

The historical SDK parameter schema has limits. File-domain inference requires
WhiteLevel exactly 4095, 16383 or 65535; arbitrary saturation levels cannot
unambiguously identify a count domain in its 16-bit TIFF containers. The
integer black-level struct cannot represent fractional black levels. Metadata
parsing rejects these cases, unsupported CFA layouts and nonempty OpcodeList2
layouts other than four equal-sized GainMap records. It returns failure rather
than assuming GBRG, truncating fractional calibration or silently dropping an
unsupported OpcodeList2. Native parameter readers must start with initialized
parameters and destroy their owned payloads, as before.
The CFA palette must use canonical RGB plane order; remapping matrices and
white balance for permuted color palettes is outside this port.

For exact typed calibration and broader opaque opcode preservation, use the
[editable RAW tools](stock-editable-raw.md). Those tools carry an explicit
sample domain and preserve metadata tag bytes. Their video/GPR adapter format
limits remain separate from this SDK format port. Generic SDK metadata,
fractional/nonstandard-domain inference and restored still-tier approval remain
accounted for in the feature rebuild; this port does not claim those are done.

## Validation

`raw_media` independently constructs calibrated source DNGs for all twelve
word formats. SDK DNG-to-GPR payloads must match the independent historical Q3
fixture byte for byte after equivalent sample-domain/phase changes. Every
decoded sample is checked against that reference. Independent TIFF reads check
physical black levels, WhiteLevel, CFA, orientation, both matrices/illuminants,
camera neutral, noise and distinct spatial GainMaps with mixed flags. Tests
also cover uncompressed DNG rewrites, editable DNG output, raw CLI format and
padded-pitch selection, both packed-12 inputs, repeated black levels and
explicit metadata rejection.

A public-header SDK probe covers all fourteen format selections, exact payload
wrap/unwrap, compressed GPR versus uncompressed DNG, decoded equality, short
input rejection and DNG-to-VC5 equality with a fresh stock encode. The media
test additionally exercises the legacy RGB path with owned calibration data.
The full checks run with ASan/UBSan; thumbnail coverage exposed inherited signed
JPEG bit shifting and null arithmetic for empty DNG blocks, which are fixed.

Local macOS Release and ASan/UBSan pass all nine CTest targets. SDK builds with
only the encoder or only the decoder also compile successfully; those build
checks do not claim runtime coverage of every API in each reduced profile.
For implementation `5feae29`, Pi 5 Release and ASan/UBSan with leak detection
pass all nine targets. The complete sanitizer run took 946.37 seconds, with
852.52 seconds in the many isolated SDK media subprocesses. The earlier
180-second timeout is not counted as a pass. The [Pi receipt](stock-still-sdk-pi5.json)
checks 381 source/test/build files against that Git revision.

The [still preset workflow](stock-still-quality.md) now exposes all six stock
presets and measured size/fidelity evidence with exact typed calibration.
Legacy defaults stay Q3 normally and Q1 with `fast_encoding`. Recreating the
old restored tiers still requires compatible CNN selection and quality gates.
