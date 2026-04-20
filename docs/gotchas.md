# Known Gotchas and Integration Notes

This document covers non-obvious behaviors, edge cases, and things to be aware of when integrating GPR v2.0.

## 1. Rounding Difference: +/- 1 LSB on ~12% of Pixels

When decoding **existing** GPR files (produced by GoPro cameras) with the v2.0 decoder, approximately 12% of pixels differ by exactly +1 LSB compared to the original decoder output. This is a systematic rounding difference, not a bug.

**Root cause**: The v2.0 branch modifies code in the same translation units as the existing inverse wavelet transform and dequantization. This changes the compiler's optimization decisions (function inlining, register allocation), which alters floating-point rounding in the `UncompandedValue()` cubic computation. The effect is deterministic and consistent.

**Impact**: The maximum error is 1 DN (digital number) out of 16,383 for 14-bit data — a 0.006% deviation. This is:
- Below the sensor noise floor (typically 2-10 DN)
- Below the quantization step (typically 4-32 DN)
- Visually imperceptible in any rendered image
- Consistent with what any source-level change to the decoder would produce

**Mitigation**: If bit-exact backward compatibility is critical, the v2.0 features can be compiled as a separate library that shares no translation units with the existing decoder.

## 2. Per-Band VLC/ANS Auto-Selection

The encoder automatically selects VLC or ANS for each wavelet band based on whichever produces smaller output. This means a single GPR file may contain a mix of VLC and ANS coded bands.

The `BandCodingMethod` tag is written only for ANS bands. Bands without this tag use VLC (the default). Old decoders will ignore the tag (it's marked as optional in the bitstream) and attempt VLC decoding on ANS bands, which will produce corrupted output for those bands.

**Backward compatibility**: GPR v2.0 files require a v2.0 decoder when any band uses ANS. The file is still a valid DNG container — metadata, thumbnails, and previews work in any DNG reader.

## 3. FormatVersion Tag Not Written

The encoder was designed to write `CODEC_TAG_FormatVersion = 0x0200` to identify v2.0 bitstreams. Due to a pointer bug in the original implementation (writing to `&bitstream` instead of `bitstream`), this tag was never actually emitted. Fixing the pointer and enabling the tag shifts subsequent bitstream data by 4 bytes, which the existing tag parser cannot handle.

The tag is disabled pending a parser update. v2.0 files are identified by the presence of `BandCodingMethod` tags.

## 4. 16-bit Mode Uses Negative Quantization Sentinel

For 16-bit data (Hasselblad X2D, etc.), the encoder writes negative quantization values to the bitstream. The decoder interprets `quant < 0` as "skip cubic uncompanding, multiply by |quant| only." This sentinel value flows through the existing quantization tag and does not require new bitstream syntax.

An old decoder encountering a negative quant will either:
- Treat it as unsigned (very large quant) — producing saturated output
- Reject it — failing gracefully

## 5. noise_restore Adds Noise to ANY File with DNG NoiseProfile

The `gpr_convert_gpr_to_raw_ex()` function applies noise restoration to decoded images when the GPR file contains a DNG `NoiseProfile` tag and a `GPRNoiseSeed` in XMP. Original GoPro files have `NoiseProfile` but not `GPRNoiseSeed`, so noise restoration is not triggered.

However, if a third-party tool writes both tags, noise will be added during decode. The CLI tool (`gpr_tools`) only routes through `_ex` when the user explicitly enables `-R` (noise replace) or `-F` (FPN calibration).

## 6. Noise-Aware Quantization Can Increase File Size

At low ISO (64-400) on detail-rich scenes, the noise-aware quantization may increase per-band quant by less than the ANS coding overhead (frequency tables, blob headers). The per-band auto-selector handles this by falling back to VLC when it would be smaller, but the overall file may still be 0-3% larger than pure VLC for low-ISO images.

At ISO 1600+, ANS+DN consistently outperforms VLC by 20-40%.

## 7. LTO Changes Floating-Point Behavior

The CMakeLists.txt enables Link-Time Optimization (LTO) when supported. LTO allows the compiler to inline functions across translation units, which can change floating-point rounding for the existing inverse wavelet code. This is the primary cause of the +/- 1 LSB difference described in item 1.

If deterministic output relative to the original codec is required, build without LTO:
```
cmake .. -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
```

## 8. NEON Paths in Existing Decoder Code

The original GoPro codebase contains `#if ENABLED(NEON)` guards in `inverse.c`, `dequantize.c`, and `raw.c`. These paths were dormant (NEON was never defined). The v2.0 CMakeLists enables `NEON=1` on ARM64, activating these paths.

The NEON paths are functionally correct but may produce different rounding than the scalar paths due to NEON's flush-to-zero behavior and different instruction scheduling. To match original output exactly, build with `-DNEON=OFF`.

## 9. Scope of Changes

This branch modifies 44 source files and adds 38 new files. The changes span the entire codec:
- Encoder: parallel wavelet transform, ANS pre-encoding, noise estimation
- Decoder: ANS dispatch, negative quant sentinel, NEON dequantization
- Common: ANS coder, noise model, FPN calibration, companding LUT
- SDK: noise pipeline, 16-bit support, XMP metadata
- Tools: CLI flags, batch encoder, quality metrics, fuzz target

Reviewing the full diff as a single PR is challenging. The logical grouping is:
1. 16-bit pixel format infrastructure
2. Noise model + FPN calibration
3. ANS entropy coder (standalone, well-tested)
4. Encoder/decoder pipeline integration
5. SDK + CLI tools
6. Documentation

## 10. Thread Safety

The encoder uses pthreads for parallel wavelet transform (Phase 1) and parallel ANS pre-encoding (Phase 1.8). Each thread operates on independent channel data with no shared mutable state. If pthread_create fails, the work runs inline on the main thread.

The decoder uses pthreads for the inverse wavelet transform. The ANS decode is single-threaded (bands are decoded sequentially from the bitstream).

The `UncompandedValueFast` LUT and `g_cubic_inv` encoder table are initialized once (with benign races on the flag check). They are read-only after initialization.
