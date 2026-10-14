# Legacy DNG/GPR Extensions

This guide summarizes the current legacy VC5 writer and compatibility
reader. The historical filename does not imply a complete v2 format
standard. It is separate from [FUSED](SPEC.md) and [GVID](GVID_CONFORMANCE.md).

## Container and input

The stills SDK stores compressed VC5 image data in a DNG/TIFF-based GPR
container. Reading TIFF metadata is not the same as decoding the image;
generic DNG readers need not support these codec extensions. Metadata
preservation must be checked on representative inputs, not assumed for
every DNG tag or camera.

[vc5_encoder.h](../source/lib/vc5_encoder/vc5_encoder.h) defines the exact
input enum: unpacked RGGB/GBRG/GRBG/BGGR at 12/14/16 bits, plus historical
packed-12 RGGB/GBRG variants. See [Stills](STILLS.md) for workflow limits.
The codec is lossy even when input and output use sixteen-bit samples.

## Current writer

[encoder.c](../source/lib/vc5_encoder/encoder.c) pre-encodes candidate
highpass bands when ANS is enabled. Bands selected for ANS emit optional
`CODEC_TAG_BandCodingMethod` (200) with value **4**, raw quantized
coefficients with four-way interleaved joint rANS, for **all supported
input bit depths**. Bands where VLC is selected omit the tag and use
method 0. Enabling ANS does not force every band to use ANS.

An ANS codeblock is segment-aligned and carries a 32-bit big-endian blob
length followed by the blob bytes, inside normal VC5 subband framing.
Do not confuse that outer length with internal ANS serialization.

Emission of optional `CODEC_TAG_FormatVersion` (201, proposed value
`0x0200`) is **disabled**. The decoder treats it as **informational**
when present; feature dispatch uses individual coding tags, not a
version-tag compatibility gate.

## Reader compatibility

[decoder.c](../source/lib/vc5_decoder/decoder.c) retains older coding paths:

| Method | Interpretation | Current writer |
|---|---|---|
| 0 / absent | VLC | Yes, including per-band fallback |
| 1 | Joint ANS with companding | No |
| 2 | Joint ANS, raw coefficients | No |
| 3 | Four-way joint ANS with companding | No |
| 4 | Four-way joint ANS, raw coefficients | Yes, at 12/14/16 bits |

Older VC5 readers cannot be assumed to decode ANS bands merely because
they can skip optional tags. This guide makes no byte-identity or universal
quality guarantee for legacy output.

## Exact definitions and limits

- [codec.h](../source/lib/vc5_common/codec.h): coding-method and version tag constants.
- [ans_joint.h](../source/lib/vc5_common/ans_joint.h) and [ans_joint.c](../source/lib/vc5_common/ans_joint.c): joint symbols, interleaved and stripe variants, exact blob serialization.
- [gpr.h](../source/lib/gpr_sdk/public/gpr.h): still conversion API.

Quantization values are carried by standard per-band quantization tags.
Noise-aware processing and metadata handling are implementation features,
not a guarantee of calibrated noise reconstruction or preservation of every
source property. Use matched codec/model evidence for quality claims;
see [Product Details](PRODUCT_DETAILS.md).

The [historical format document](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/format-spec-v2.md)
preserves earlier tables and investigations. They are not a current wire
contract; this summary leaves exact extension layouts to the linked source.
