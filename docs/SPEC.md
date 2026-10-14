# FUSED Payload Reference

This is an implementation guide, not a complete independent wire specification.
FUSED is distinct from legacy VC5 in TIFF/GPR stills and from the enclosing
[GVID container](GVID_CONFORMANCE.md).

## Header and profiles

[fused_encode.h](../source/lib/vc5_encoder/fused_encode.h) defines
`FUSED_HEADER`: twelve uint32 fields, 48 bytes, followed by `num_bands`
uint32 byte lengths and the corresponding band payloads. The supported
implementation uses little-endian integers. Magic is `0x44535546` (`FUSD`);
`FUSED_VERSION` is 1.

Fields in order are `magic`, `version`, `width`, `height`, `pixel_format`,
`quality`, `is_rggb`, `log_bits`, `prescale`, `multi_level`, `num_bands`,
and `decimate`. The `multi_level` field is a profile/depth selector:

| `multi_level` | Bands | Interpretation |
|---|---:|---|
| 0 | 16 | Single level with LL; four bands per channel |
| 1 | 40 | Legacy boolean encoding of three levels; reader compatibility |
| 2 | 28 | Two levels; seven bands per channel |
| 3 | 40 | Three levels; ten bands per channel |

The current multilevel writer emits 2 or 3. Single-level 12-band output
without LL is not reconstructable and is rejected by the decoder.
For exact profile dispatch, see
[fused_encode.c](../source/lib/vc5_encoder/fused_encode.c) and
[fused_decode.c](../source/lib/vc5_decoder/fused_decode.c) define current dispatch.

## Samples and transforms

Input is unpacked uint16 Bayer. Pixel-format tags are `0=RGGB12`,
`1=RGGB14`, `2=GBRG12`, `3=GBRG14`, `4=RGGB16`, `5=GBRG16`.
The stills API uses a different enum and additionally supports GRBG/BGGR;
do not copy its enum values into FUSED headers.

The encoder applies the log curve to samples before constructing the four
GS/RG/BG/GD channels, then performs wavelet decomposition, quantization,
and band entropy coding. Twelve- and fourteen-bit input use a 14-bit log
domain with default level-1 prescale **3**. Sixteen-bit input uses a 16-bit
log domain with default prescale **2**. Overrides exist; read the stored
prescale rather than assuming the default.

`decimate=2` indicates channel-space decimation: full decode returns Bayer
dimensions `width/2` by `height/2`. Values 0 and 1 mean no decimation.
The multilevel half-resolution API reduces these dimensions again;
single-level full decode does not honor that half-resolution flag.
See [fused_decode.h](../source/lib/vc5_decoder/fused_decode.h) for entry points,
including the separate single-level LL preview API, and the implementation
for dimension rounding and supported combinations.

## Bands and extensions

Band ordering is channel-major. Single-level slots are LL1, LH1, HL1,
HH1. Two-level slots are LH1/HL1/HH1, LH2/HL2/HH2, LL2. Three-level
slots add LH3/HL3/HH3 before LL3. Zero-length bands represent zero bands.

[ans_joint.h](../source/lib/vc5_common/ans_joint.h) defines joint rANS
interfaces and stripe framing; [ans_joint.c](../source/lib/vc5_common/ans_joint.c)
defines exact serialization. Not every nonempty band is an ordinary rANS
blob: raw-LL `FLL1` and delta-LL `FLL2` variants are implemented in the
fused encoder/decoder. Their markers, lengths, and decode rules are defined
there, not fully specified here.

The header is not a complete record of encoder configuration. It stores
a quality index, not all effective quantization divisors. Quant overrides,
rate-control scale, and deeper-level tuning can require matching settings
outside the payload. Do not infer that arbitrary tuning is self-contained
or portable. Consult the implementations for exact quant tables, extension
behavior, and parser limits.

## Related references

- [Legacy DNG/GPR extensions](format-spec-v2.md)
- [Architecture](architecture.md) and [Capture API](CAPTURE_API.md)
- [Historical specification and tuning](https://github.com/dcliftreaves/gpr/blob/f13dbe95d3e97c400e8d493ba178dbb037ff3c96/docs/SPEC.md), not the current contract.
