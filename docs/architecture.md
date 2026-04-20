# GPR v2.0 Codec Architecture

## What This Is

GPR is a raw image compression codec built on GoPro's open-source VC5/CineForm wavelet codec. The v2.0 extensions add noise-aware compression and adaptive entropy coding, targeting three camera systems: GoPro (12MP, 14-bit), Nikon Z8 (45MP, 14-bit), and Hasselblad X2D (100MP, 16-bit).

The output is a DNG-compliant container with VC5 compressed image data. Any DNG reader can open the file; only decompression requires a v2.0 decoder.

---

## The Pipeline

### Encoding

```
Raw Bayer → Noise Remove → Unpack to 4 Components → Wavelet Transform → Quantization → ANS Entropy Coding → Bitstream
              (optional)        (R, Gr, Gb, B)        (3-level, 2/6 biorthogonal)                    (Joint RLV)
```

The encoder runs in four phases:

**Phase 0.5: Noise Estimation + Adaptive Quantization**

Before the wavelet transform, the encoder estimates noise from each Bayer channel using signal-dependent MAD (Median Absolute Deviation). Pixels are binned by brightness into 4 levels, and the noise sigma is estimated from the middle bins (25th-75th percentile) to avoid bias from dark pixels where the log curve amplifies noise.

This sigma feeds into per-band quantization: for each of the 9 highpass wavelet subbands, the quantization divisor is raised to at least `sigma × filter_gain / prescale_divisor`. The effect is that the codec's own quantization removes noise natively — no post-processing needed.

**Why this works and nothing else does**: VC5 fuses the wavelet filter with quantization in a single pass. Post-transform denoise has zero effect because the coefficients are already quantized. Pixel-domain noise removal (the "Jetraw approach") actually *hurts* VC5 — quantized residuals are less compressible than Gaussian noise for wavelet codecs. Pre-transform adaptive quantization is the only approach that works within VC5's architecture.

**Phase 1: Parallel Wavelet Transform**

Four pthread threads run the 3-level 2/6 biorthogonal wavelet transform simultaneously, one per Bayer channel. Each level applies horizontal then vertical filtering with prescale (right-shift to prevent overflow). The prescale values differ by bit depth: {0,2,2} for 14-bit, {2,3,3} for 16-bit.

**Phase 1.8: Parallel ANS Pre-encoding**

After the wavelet transform completes, four more threads pre-encode all ANS band data in parallel. Each thread processes all 9 highpass bands for one channel: cubic companding (14-bit only), frequency table construction, and rANS encoding. The pre-encoded blobs are stored in `encoder->preencoded_band[channel][level][band]`.

This was the single largest performance optimization. It moves the entire ANS encode off the serial bitstream path — Phase 2 just copies pre-encoded bytes.

**Phase 2: Serial Bitstream Assembly**

Writes channel headers, lowpass bands, and pre-encoded highpass bands to the bitstream sequentially. The bitstream format is tag-value pairs (VC5 standard) with the ANS-coded band data embedded as byte blobs.

### Decoding

```
Bitstream → ANS Decode → Dequantize (+ Uncompand) → Inverse Wavelet → Pack to Bayer → Noise Restore
                                                       (3-level, parallel)                  (optional)
```

The decoder mirrors the encoder. Key differences:
- ANS decoding is serial (one band at a time, determined by bitstream order)
- The inverse wavelet transform runs in 4 parallel threads
- Noise restore adds statistically equivalent noise back from a deterministic PRNG

---

## The Entropy Coder: Joint RLV ANS

### Why Not VLC?

The original VC5 codec uses a fixed Huffman codebook (Table 17) designed for GoPro's typical coefficient distribution. It works well for GoPro but can't adapt to different cameras, ISOs, or noise levels. For the Z8 and X2D, the coefficient statistics differ enough that VLC wastes 20-40% of bits.

### How Joint RLV Works

Each wavelet coefficient is encoded as a single (run_class, magnitude_class) token, with residual bits for exact values and sign. This replaces the original separate-stream approach (one ANS stream for runs, another for magnitudes) which required 2 ANS operations per coefficient.

**10 run classes**: 0-3 are direct (zero extra bits). 4-9 use 2-7 extra bits for exponentially increasing ranges (4-7, 8-15, 16-31, ..., 128-255).

**16 magnitude classes**: 0-7 are direct. 8-15 use 3-10 extra bits for ranges (8-15, 16-31, ..., 1024+).

**160 joint symbols** (10 × 16) are entropy-coded with one rANS operation. The residual bits (run extra + mag extra + sign) are packed into a separate bitstream. This design concentrates the entropy model on the coarse structure (which run/mag class?) while using raw bits for the fine detail (exact value within class).

### 4-Way Interleaved rANS

Tokens are distributed round-robin across 4 rANS state machines. This reduces pipeline stalls by interleaving independent state updates. The encoder processes tokens in reverse (standard for rANS), assigning token i to state (i % 4). The decoder reads in forward order from the same round-robin assignment.

The 4-way interleave is the foundation for NEON vectorized decode (not yet implemented — the scalar fast-path already captures most of the gain).

### 14-bit vs 16-bit Modes

**Mode 3 (14-bit companded)**: Wavelet coefficients are cubic-companded from [0,1023] to [0,255] before Joint RLV encoding. The decoder applies inverse companding (UncompandedValueFast LUT) during dequantization. Companding bounds the coefficient range, improving entropy coding efficiency.

**Mode 4 (16-bit raw)**: Coefficients are passed directly to Joint RLV with no companding. The encoder signals this by writing a negative quantization value to the bitstream. The decoder sees `quant < 0` and skips the uncompanding LUT, multiplying by |quant| directly.

---

## Noise-Aware Compression

### The Problem

Sensor noise is signal-dependent: `variance = scale × signal + offset` (Poisson-Gaussian model). At high ISO, noise accounts for 60-80% of the file's entropy. A codec that treats noise as signal wastes bits preserving random values that carry no visual information.

### The Solution: Three Layers

**Layer 1: Pixel-Domain Noise Removal** (`noise_remove`)

Optional. Before encoding, each pixel is quantized to its noise floor: `quantized = round(signal / sigma) × sigma`. This produces a cleaner signal with dramatically lower entropy. The error is bounded by ±sigma/2 per pixel, which is below the noise floor by definition.

Uses a precomputed sqrt LUT (every 16th signal level) to avoid per-pixel sqrt calls. The LUT is 4KB — fits in L1 cache.

**Layer 2: Wavelet-Domain Adaptive Quantization** (Phase 0.5)

Always active when denoise is enabled. Raises per-band quantization to the noise floor. BayesShrink adapts the threshold per band: signal-rich bands (detail) get lower thresholds, noise-dominant bands get higher thresholds. Falls back to VisuShrink for pure-noise bands.

**Layer 3: Pixel-Domain Noise Restoration** (`noise_restore`)

Optional. After decoding, adds back statistically equivalent noise using a deterministic PRNG seeded from the original image data. The noise has the correct per-pixel variance from the Poisson-Gaussian model. Uses triangular distribution (sum of 2 uniforms) which is perceptually indistinguishable from Gaussian and requires only 2 xorshift operations per pixel.

### Does This Actually Help?

Yes, significantly — but only at high ISO where noise dominates:

| ISO Range | ANS+DN vs VLC | Why |
|-----------|---------------|-----|
| 64-200 | -8% to +19% | Low noise, VLC is already near-optimal |
| 400-800 | -3% to +8% | Transition zone |
| 1600+ | +20% to +29% | Noise removal creates large zero runs |

The noise-aware quantization can never *reduce* quality below the selected quality preset — it only increases quant when the noise floor exceeds the default quant.

---

## Performance

All measurements on Apple Silicon (M-series), single image, wall clock time.

### Speed

| Camera | Operation | Time | Throughput |
|--------|-----------|------|-----------|
| GoPro 12MP | Encode (ANS+DN) | 0.07s | 170 MP/s |
| GoPro 12MP | Decode | 0.12s | 100 MP/s |
| Z8 45MP | Encode (ANS+DN) | 1.0s | 45 MP/s |
| Z8 45MP | Decode | 0.5s | 90 MP/s |
| X2D 100MP | Encode (ANS+DN) | 2.5s | 40 MP/s |
| X2D 100MP | Decode | 1.2s | 83 MP/s |

DNG parsing dominates encode time (29-40%). The actual codec work is ~60% of the total.

### Compression

| Camera | Raw Size | VLC | ANS+DN | Ratio |
|--------|----------|-----|--------|-------|
| GoPro Hero6 | 24 MB | 5.3 MB | **3.1 MB** | **7.7x** |
| Nikon Z8 | 91 MB | 15.5 MB | **16.5 MB** | **5.5x** |
| Hasselblad X2D | 204 MB | — | **40.9 MB** | **5.0x** |

GoPro benefits most from Joint RLV (39% better than VLC). Z8 at low ISO is slightly larger than VLC — the adaptive coding overhead doesn't pay for itself on clean, detail-rich images. At ISO 1600+, ANS+DN consistently beats VLC by 20-29%.

### Where Time Goes (Z8 Encode Profile)

| Component | % of Total |
|-----------|-----------|
| DNG parsing (LJPEG decode) | 29% |
| ANS encoding (Joint RLV) | 31% |
| ANS table construction | 9% |
| Wavelet transform | 1% (parallel) |
| Noise estimation | 2% |
| Image unpacking | 2% |
| Bitstream I/O | 2% |

### Where Time Goes (Z8 Decode Profile)

| Component | % of Total |
|-----------|-----------|
| Noise restore (PRNG + LUT) | 53% |
| ANS decoding (Joint RLV) | 20% |
| Pack components to RAW | 6% |
| Inverse wavelet transform | 6% (parallel) |

---

## Validation

### Mass Scan Results

| Camera | Files Tested | Failures | Avg Compression |
|--------|-------------|----------|-----------------|
| Nikon Z8 | 153 | 0 | 7.0x |
| Hasselblad X2D | 4 | 0 | 5.0x |
| GoPro (Hero5-10) | 6 | 0 | 4.6x |

Previous scan (before Joint RLV): 6,713 Z8 files with 2 outliers at 43.5 dB PSNR (high-ISO images, working as designed).

### Quality Verification

ANS encoding is **bit-exact lossless** relative to VLC at the same quantization settings. The only quality difference comes from noise-aware quantization (which trades PSNR for compression at the noise floor):

| Mode | PSNR vs Original | SSIM | Notes |
|------|-----------------|------|-------|
| VLC | 55.6 dB | 0.99984 | Baseline |
| ANS (same quant) | 55.6 dB | 0.99984 | Identical to VLC |
| ANS+DN | 50.7 dB | 0.99933 | Noise-aware quant active |

---

## Decisions and Tradeoffs

### Why Joint RLV Instead of Separate Run + Magnitude ANS

The original approach used two separate ANS streams — one for zero runs, one for magnitudes. This required 2 ANS encode/decode operations per coefficient, 2 frequency tables per band, and a separate sign bitstream.

Joint RLV combines run class and magnitude class into a single symbol, requiring only 1 ANS operation per coefficient. The compression improvement is 5-10% for 14-bit data, and the code is 70 lines simpler.

The tradeoff: the joint frequency table has 160 symbols instead of ~256, which means slightly less precise frequency modeling for individual run/magnitude distributions. In practice, the single-operation efficiency more than compensates.

### Why Interleaved Rather Than True Parallel rANS

True SIMD rANS would decode 4 independent bands simultaneously. But wavelet bands have different sizes and statistics, making load balancing difficult. Interleaving within a single band is simpler and provides a uniform 4-way pipeline without load imbalance.

The main limitation: all 4 states share a single byte stream for renormalization, so the renorm reads are sequential. Future NEON optimization would vectorize the table lookup and state arithmetic (which are truly parallel) while keeping renorm serial.

### Why Triangular Noise Instead of Gaussian

The noise restore uses a triangular distribution (sum of 2 uniform samples) instead of Gaussian. This requires 2 xorshift operations per pixel instead of 12 (CLT) or 2 + transcendentals (Box-Muller). The triangular distribution has heavier tails than Gaussian but is perceptually indistinguishable at the noise levels we add (half-sigma). The 2x speed improvement in noise_restore (53% of decode time) is significant.

### Why Not Wider Companding for 16-bit

The X2D's mode 4 (raw 16-bit) achieves 5.0x compression without companding. Wider companding (expanding the cubic curve from [0,255] to [0,1023]) would bound the coefficient range but adds complexity and only offers an estimated <5% gain. The Joint RLV class structure already handles large magnitudes efficiently with its exponential class boundaries.

### Why the FormatVersion Tag Is Disabled

The v2.0 encoder was supposed to write `CODEC_TAG_FormatVersion = 0x0200` to the bitstream header. Due to a pointer bug (`&bitstream` instead of `bitstream`), this tag was never actually written to the stream — it corrupted an unrelated memory location. The bug was latent for months.

Fixing the pointer and enabling the tag shifts all subsequent bitstream data by 4 bytes, which breaks the decoder's tag parser (the VC5 tag parser is position-sensitive). The correct fix requires updating the decoder's tag dispatch to handle the new position, which is deferred to v3.0.

---

## Known Issues

1. **No .fff/.3FR support**: Hasselblad raw files require manual conversion to DNG via rawpy or Adobe DNG Converter
2. **FormatVersion tag disabled**: Deferred to v3.0 (see above)
3. **No NEON vectorization in rANS decode**: The x4 interleave foundation is in place but the inner loop is scalar
4. **DNG parsing is 29% of encode time**: Would need DNG SDK replacement for further speedup
5. **Noise restore is 53% of decode time**: Further optimization requires NEON vectorization of the LUT + PRNG loop
6. **noise_model.h exposed in public API**: `gpr.h` includes `noise_model.h` directly, leaking `fpn_model` struct internals

---

## File Map

| File | Lines | Purpose |
|------|-------|---------|
| `source/lib/vc5_encoder/encoder.c` | 2900 | Encoder pipeline: phases 0.5 → 1 → 1.8 → 2 |
| `source/lib/vc5_decoder/decoder.c` | 2500 | Decoder pipeline: ANS dispatch, band decode |
| `source/lib/vc5_common/ans_joint.c` | 630 | Joint RLV ANS: encode/decode, x4 interleaved |
| `source/lib/vc5_common/ans_joint.h` | 80 | Joint RLV types: JANS_TABLE, class constants |
| `source/lib/vc5_common/ans.c` | 400 | Legacy separate run+mag ANS (still used for table serialization) |
| `source/lib/vc5_common/noise_model.c` | 550 | Noise remove/restore, FPN model, PRNG |
| `source/lib/vc5_encoder/denoise.c` | 575 | BayesShrink, MAD estimation, adaptive quant |
| `source/lib/vc5_decoder/dequantize.c` | 160 | Inverse quantization, negative quant sentinel, NEON |
| `source/lib/vc5_decoder/raw.c` | 330 | Component clamping, Bayer packing |
| `source/lib/gpr_sdk/private/gpr.cpp` | 2260 | SDK: DNG I/O, noise pipeline integration |
| `source/app/gpr_tools/main.cpp` | 350 | CLI tool: -A, -D, -R, -F flags |
| `source/app/compare_quality/compare_quality.c` | 240 | PSNR, SSIM, noise preservation metrics |
| `source/app/ans_test/ans_test.c` | 195 | ANS round-trip unit test |
| `source/app/fuzz_ans/fuzz_ans.c` | 92 | libFuzzer target for ANS decode |
| `docs/format-spec-v2.md` | 440 | Complete v2.0 specification |
| `docs/future-ideas.md` | 28 | GPU accel, streaming, lossless mode |
| `.github/workflows/ci.yml` | 80 | CI: build + smoke test on Linux + macOS |
