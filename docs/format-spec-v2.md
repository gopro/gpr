# GPR Format Specification v2.0

## Overview

GPR v2.0 extends the GoPro Raw (GPR) format with noise-aware adaptive quantization and ANS entropy coding. GPR files are DNG-compliant containers with VC5/CineForm compressed image data.

A GPR v2.0 file is a valid DNG file. Any DNG-compatible reader can open the container and read metadata. Only the VC5 bitstream decompression requires a v2.0-aware decoder.

## Scope

### Supported Cameras
- Any camera producing DNG-compatible raw data, including:
  - 12-bit sensors (e.g., GoPro Hero5/6, Fusion)
  - 14-bit sensors (e.g., GoPro Hero7-12, 45MP DSLRs via DNG conversion)
  - 16-bit sensors (e.g., 100MP medium format, 11664x8750)

### Bit Depths
- 12-bit, 14-bit, 16-bit per sample
- Bayer CFA patterns: RGGB, GBRG (2x2 only)

---

## File Structure

A GPR file is a DNG/TIFF container:

```
[TIFF Header]
[IFD #0 — Main Image]
  ├── Standard DNG tags (Make, Model, ColorMatrix, etc.)
  ├── VC5 compressed image data (in NewSubfileType strip)
  └── XMP metadata block
       ├── GPRNoiseSeed (uint32) — PRNG seed for noise reconstruction
       ├── GPRNoiseSigma0..3 (float64) — per-channel noise sigma
       └── GPRFpnPoly_ch0..3 (string) — FPN polynomial coefficients
[IFD #1 — Thumbnail (optional)]
```

### DNG Compliance

GPR v2.0 files preserve all standard DNG tags:
- `Make`, `Model`, `UniqueCameraModel`
- `ColorMatrix1`, `ColorMatrix2`, `ForwardMatrix1`, `ForwardMatrix2`
- `CalibrationIlluminant1`, `CalibrationIlluminant2`
- `AsShotNeutral`, `AsShotWhiteXY`
- `BlackLevel`, `WhiteLevel`
- `BayerPattern` / `CFAPattern`
- `NoiseProfile` (DNG noise_scale, noise_offset)
- `BaselineExposure`, `BaselineNoise`

Non-GoPro cameras retain their original DNG metadata through the round-trip.

---

## VC5 Bitstream Extensions

### Version Identification

To enable graceful failure on old decoders, v2.0 files include an optional version tag in the VC5 bitstream header:

```
CODEC_TAG_FormatVersion = 201  (optional tag)
Value: 0x0200 (version 2.0)
```

Old decoders that don't recognize this tag skip it (standard optional tag behavior). They will encounter ANS-encoded bands and fail at the VLC decoder with a recognizable error pattern.

### Band Coding Method Tag

Each highpass subband may independently use VLC or ANS coding:

```
CODEC_TAG_BandCodingMethod = 200  (optional tag, per-band)
Value: 0 = VLC run-length (default, backward compatible)
       1 = ANS separate streams (legacy — supported by decoder, not written by current encoder)
       2 = Joint RLV ANS (legacy — supported by decoder, not written by current encoder)
       3 = Joint RLV ANS, 4-way interleaved, companded (≤14-bit) — CURRENT
       4 = Joint RLV ANS, 4-way interleaved, raw (16-bit) — CURRENT
```

The current encoder writes mode 3 for ≤14-bit data and mode 4 for 16-bit data. Modes 1 and 2 are accepted by the decoder for backward compatibility with files encoded by earlier versions. When this tag is absent, the band uses VLC (full backward compatibility with v1.0).

The encoder auto-selects VLC or ANS per-band based on compressed size — the tag is only written for bands where ANS wins.

### ANS Band Data Format (Modes 1-4)

All ANS modes use the same Joint RLV blob format (modes 1 used a separate-stream format in earlier versions but is no longer written). The band codeblock contains:

```
[AlignBitsSegment]
[blob_size: 32 bits, big-endian]       — byte count of the Joint RLV ANS blob
[blob_data: blob_size bytes]           — self-describing Joint RLV blob (see below)
```

The band codeblock is still wrapped in the standard `PushSampleSize`/`PopSampleSize` pair, so legacy parsers can skip it by size even if they can't decode the contents.

#### ANS Frequency Tables

Two tables per band: run-length distribution and magnitude distribution.

```
Table format: ANS_NUM_SYMBOLS × 2 bytes per table × 2 tables
  = (ANS_MAX_SYMBOL + 2) × 2 × 2 = 516 × 2 = 1032 bytes per band

Byte layout:
  [run_freq[0]: 16 bits big-endian] ... [run_freq[ANS_NUM_SYMBOLS-1]]
  [mag_freq[0]: 16 bits big-endian] ... [mag_freq[ANS_NUM_SYMBOLS-1]]

Frequencies sum to ANS_TABLE_SIZE (2048).
Every frequency must be >= 1 for used symbols.
```

Constants:
- `ANS_TABLE_BITS = 11`
- `ANS_TABLE_SIZE = 2048`
- `ANS_MAX_SYMBOL = 256`
- `ANS_NUM_SYMBOLS = 258` (0..256 + escape)

#### ANS Coded Data

The rANS-encoded bitstream contains interleaved run-length and magnitude symbols:

```
Coded data format:
  [pair_count: 32 bits big-endian]     — number of (run, magnitude) pairs
  [rans_size: 32 bits big-endian]      — byte count of rANS data
  [sign_bytes: 32 bits big-endian]     — byte count of sign bitstream
  [rans_data: rans_size bytes]         — rANS state + renormalization bytes
  [sign_data: sign_bytes bytes]        — packed sign bits (1 bit per non-zero value)
```

Each pair encodes:
1. A zero run-length (rANS symbol from run table)
2. A magnitude (rANS symbol from magnitude table)
3. If magnitude > 0: a sign bit from the sign bitstream

Magnitudes are cubic-companded before ANS encoding (same `ComputeCubicTable` curve as VLC) to produce values in [0, 255].

#### rANS Parameters

- Byte-aligned rANS (Giesen-style)
- `RANS_BYTE_L = 1 << 23` (renormalization threshold)
- Encoder writes forward, buffer is reversed
- Initial state: `RANS_BYTE_L`
- Final state flushed as 4 bytes (little-endian, then reversed with buffer)
- Decoder reads state from first 4 bytes (big-endian after reversal)

### Joint RLV ANS Band Data Format (Mode 2)

When `CODEC_TAG_BandCodingMethod = 2`, the band uses joint run-length/value ANS coding. Instead of two separate symbol streams (run and magnitude), mode 2 encodes a single joint symbol per coefficient combining the run class and magnitude class.

Mode 2 is selected for all cameras when the `-A` flag is passed to the encoder.

#### Joint Symbol Classes

**Run classes** (10 classes, `JANS_RUN_CLASSES = 10`):

| Class | Base value | Extra bits |
|-------|-----------|------------|
| 0 | 0 | 0 |
| 1 | 1 | 0 |
| 2 | 2 | 0 |
| 3 | 3 | 0 |
| 4 | 4 | 2 |
| 5 | 8 | 3 |
| 6 | 16 | 4 |
| 7 | 32 | 5 |
| 8 | 64 | 6 |
| 9 | 128 | 7 |

Run class values are contiguous: the base values are `{0, 1, 2, 3, 4, 8, 16, 32, 64, 128}` with `{0, 0, 0, 0, 2, 3, 4, 5, 6, 7}` extra bits respectively.

**Magnitude classes** (16 classes, `JANS_MAG_CLASSES = 16`):

| Class | Base value | Extra bits |
|-------|-----------|------------|
| 0 | 0 | 0 |
| 1 | 1 | 0 |
| 2 | 2 | 0 |
| 3 | 3 | 0 |
| 4 | 4 | 0 |
| 5 | 5 | 0 |
| 6 | 6 | 0 |
| 7 | 7 | 0 |
| 8 | 8 | 3 |
| 9 | 16 | 4 |
| 10 | 32 | 5 |
| 11 | 64 | 6 |
| 12 | 128 | 7 |
| 13 | 256 | 8 |
| 14 | 512 | 9 |
| 15 | 1024 | 10 |

The base values are `{0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024}` with `{0, 0, 0, 0, 0, 0, 0, 0, 3, 4, 5, 6, 7, 8, 9, 10}` extra bits respectively.

#### Joint Symbol Encoding

Each (run, magnitude) pair is encoded as a single joint symbol:

```
joint_symbol = run_class × JANS_MAG_CLASSES + mag_class
```

Total joint symbols: `JANS_NUM_SYMBOLS = JANS_RUN_CLASSES × JANS_MAG_CLASSES = 10 × 16 = 160`

#### ANS Table Parameters (Mode 2)

- `ANS_TABLE_BITS = 11`
- `ANS_TABLE_SIZE = 2048`
- Single frequency table over 160 joint symbols

#### Blob Format (Mode 2)

The band codeblock for mode 2 contains a single self-describing blob:

```
[token_count: 32 bits]    — number of (run, magnitude) tokens
[freq_size: 32 bits]      — byte count of frequency table data
[rans_size: 32 bits]      — byte count of rANS-coded joint symbols
[resid_size: 32 bits]     — byte count of residual stream (extra bits + sign bits)
[freq_data: freq_size bytes]   — serialized joint frequency table
[rans_data: rans_size bytes]   — rANS-encoded joint symbols
[resid_data: resid_size bytes] — packed extra bits and sign bits
```

The residual stream contains, for each token in order:
1. Extra bits for the run class (if the class requires extra bits)
2. Extra bits for the magnitude class (if the class requires extra bits)
3. A sign bit (1 raw bit) for each nonzero coefficient (magnitude class > 0)

#### Inverse Quantization (Mode 2)

Mode 2 does **not** apply cubic uncompanding. The magnitude values are raw quantized coefficients, not companded indices. The encoder signals this to the decoder by using a negative quantization sentinel, which causes the decoder to skip the uncompanding step during inverse quantization.

#### rANS Parameters (Mode 2)

Same as mode 1: byte-aligned rANS (Giesen-style), `RANS_BYTE_L = 1 << 23`, encoder writes forward with buffer reversal, initial state `RANS_BYTE_L`.

---

## Noise-Aware Quantization

### Encoder Behavior

When noise-aware quantization is enabled (`-D` flag), the encoder:

1. **Estimates noise** from each channel's component array (post-log-curve, pre-wavelet):
   - 4-bin signal-level-dependent MAD estimation
   - Uses middle bins (25th-75th percentile) to avoid dark-pixel bias

2. **Propagates noise through wavelet gains**:
   - Filter gain: LH/HL = √2, HH = 2.0
   - Prescale divisor: cumulative product of `2^prescale[level]`
   - Per-band sigma = raw_sigma × filter_gain / prescale_divisor

3. **Adjusts quantization tables**:
   - `quant[band] = max(default_quant, min(noise_quant, default_quant × 3))`
   - The 3× cap prevents signal destruction on any image
   - Default quant comes from the quality preset (Q0-Q8)

### Decoder Behavior

The decoder does not need special handling for noise-aware quantization. The adjusted quant values are stored in the standard `CODEC_TAG_Quantization` tags per band. The decoder applies standard inverse quantization.

### Noise Metadata (XMP)

When noise-aware encoding is used, the following XMP properties are written:

| Property | Type | Description |
|----------|------|-------------|
| `GPRNoiseSeed` | uint32 | PRNG seed for noise reconstruction |
| `GPRNoiseSigma0` | float64 | Channel 0 noise sigma |
| `GPRNoiseSigma1` | float64 | Channel 1 noise sigma |
| `GPRNoiseSigma2` | float64 | Channel 2 noise sigma |
| `GPRNoiseSigma3` | float64 | Channel 3 noise sigma |

XMP namespace: `http://ns.adobe.com/exif/1.0/aux/`

---

## Noise Remove/Restore (Optional)

An optional pixel-domain noise separation pipeline:

### Encoder (`noise_remove`)
For each raw pixel:
1. Compute sigma from DNG NoiseProfile: `sigma = sqrt(noise_scale × signal + noise_offset)`
   - NoiseProfile values are DNG-normalized [0,1]; convert to raw units: `scale_raw = scale_dng × max_val`
2. Quantize: `output = round(signal / sigma) × sigma`
3. Store seed in XMP for decoder reconstruction

### Decoder (`noise_restore`)
For each decoded pixel:
1. Compute sigma from stored NoiseProfile (same formula)
2. Add PRNG noise: `output = signal + prng_gaussian(seed, row, col) × sigma × 0.5`
3. Clamp to valid range

The decoder automatically triggers noise restoration when `GPRNoiseSeed` is present in XMP, unless `denoise_output` mode is requested.

**Note**: Pixel-domain noise removal is only beneficial with a custom entropy coder or non-wavelet compressor. With VC5's wavelet+VLC pipeline, it increases file size because quantization steps are less compressible than Gaussian noise. Use noise-aware quantization (`-D`) instead.

---

## Quality Presets

| Preset | Name | Quant Table (14-bit scaled) |
|--------|------|----------------------------|
| Q0 | Low | {1, 21, 21, 10, 55, 55, 41, 439, 439, 658} |
| Q1 | Medium | {1, 21, 21, 10, 41, 41, 27, 219, 219, 329} |
| Q2 | High | {1, 21, 21, 10, 27, 27, 21, 110, 110, 164} |
| Q3 | Filmscan-1 | {1, 21, 21, 10, 21, 21, 10, 82, 82, 123} |
| Q4 | Filmscan-X | {1, 21, 21, 10, 21, 21, 10, 55, 55, 82} |
| Q5 | Filmscan-2 | {1, 21, 21, 10, 21, 21, 10, 27, 27, 41} |
| Q6 | Filmscan-3 | {1, 10, 10, 5, 10, 10, 5, 14, 14, 21} |
| Q7 | Filmscan-4 | {1, 5, 5, 3, 10, 10, 5, 14, 14, 21} |
| Q8 | Filmscan-5 | {1, 3, 3, 2, 9, 9, 5, 14, 14, 21} |

For 16-bit data, quant values are scaled by `12/16 = 0.75`.
For 12-bit data, quant values are unscaled (table is designed for 12-bit).

Subband mapping: `[LL, L0_LH, L0_HL, L0_HH, L1_LH, L1_HL, L1_HH, L2_LH, L2_HL, L2_HH]`

---

## ANS Auto-Selection

When the `-A` flag is set, the encoder uses Joint RLV ANS with 4-way interleaved rANS states. Mode 3 applies cubic companding (≤14-bit), mode 4 encodes raw coefficients (16-bit). The encoder auto-selects VLC or ANS per-band — if VLC produces a smaller band, no ANS tag is emitted for that band.

| Bit depth | Default (no flag) | `-A` flag |
|-----------|-------------------|-----------|
| 12-bit | Mode 0 (VLC) | Mode 3 (Joint RLV, companded, interleaved) |
| 14-bit | Mode 0 (VLC) | Mode 3 (Joint RLV, companded, interleaved) |
| 16-bit | Mode 0 (VLC) | Mode 4 (Joint RLV, raw, interleaved) |

Per-band auto-selection: the encoder tries both ANS and VLC for each band and writes whichever is smaller. The `BandCodingMethod` tag is only present for ANS bands.

---

## Backward Compatibility

| Feature | v1.0 decoder behavior | v2.0 decoder behavior |
|---------|----------------------|----------------------|
| VLC-only file | Full decode | Full decode |
| ANS mode 1 band | Fails at VLC decode | Reads ANS tag, uses ANS decoder (separate streams) |
| ANS mode 2 band | Fails at VLC decode | Reads ANS tag, uses joint RLV ANS decoder |
| Noise metadata in XMP | Ignored | Reads seed/sigma for reconstruction |
| FormatVersion tag | Skipped (optional) | Reads version, validates |

A v2.0 encoder with ANS disabled and denoise disabled produces a byte-identical file to v1.0.

---

## Reference Implementation

- Encoder: `source/lib/vc5_encoder/encoder.c` — `EncodeHighpassBand()`
- Decoder: `source/lib/vc5_decoder/decoder.c` — `DecodeHighpassBand()`
- ANS coder (mode 1): `source/lib/vc5_common/ans.c`
- Joint RLV ANS coder (modes 3/4): `source/lib/vc5_common/ans_joint.c`
- Noise estimation: `source/lib/vc5_encoder/denoise.c`
- CLI: `source/app/gpr_tools/main.cpp` — flags `-D`, `-A`, `-E`, `-R`

---

## Validation Criteria

A conforming v2.0 encoder/decoder must:

1. Round-trip any supported raw file with PSNR ≥ 40 dB (at Q3 or above)
2. Preserve all DNG metadata through the round-trip (Make, Model, ColorMatrix, NoiseProfile)
3. Produce files that open in standard DNG readers (metadata and thumbnail accessible)
4. Fail gracefully when encountering unsupported codec features (return error, don't corrupt)
5. Validate all untrusted data from the bitstream (sizes, frequencies, pair counts)

---

## Compression Results Reference

### GoPro (14-bit, `-A -D`)

| Camera | Quality | VLC | ANS+DN | vs VLC |
|--------|---------|-----|--------|--------|
| Hero6 | Q3 | 5.3MB | 3.1MB | **+41%** |
| HERO7 | Q3 | 7.7MB | 5.5MB | **+24%** |
| Hero5 | Q3 | 8.5MB | 6.0MB | **+30%** |

### 100MP 16-bit Medium Format (16-bit, `-A -D`)

| Image | ISO | VLC | ANS+DN | vs VLC |
|-------|-----|-----|--------|--------|
| Scene | 64 | 48.2MB | 41.5MB | **+14%** |
| Scene | 200 | 41.3MB | 40.9MB | **+1%** |
| Scene | 1600 | 89.6MB | 63.4MB | **+29%** |
| Scene | 3200 | 62.3MB | 49.9MB | **+20%** |
| Flat field | 800 | 11.6MB | 5.1MB | **+56%** |
| Flat field | 3200 | 31.3MB | 10.9MB | **+65%** |

### 45MP 14-bit DSLR (14-bit via DNG, `-A -D`)

| Image | ISO | VLC | ANS+DN | vs VLC |
|-------|-----|-----|--------|--------|
| Scene | 64 | 14.8MB | 18.7MB | -26% |
| Scene | 320 | 15.5MB | 16.5MB | -6% |
| Scene | 22800 | 39.5MB | 30.5MB | **+23%** |

### Key Findings

- ANS+DN advantage increases with ISO (more noise to remove adaptively)
- At ISO 1600+, ANS+DN beats VLC by 20-29% on scene photos
- At ISO 64, VLC's fixed codebook is near-optimal for detail-rich scenes
- Flat fields benefit most: up to 65% smaller at ISO 3200

### Mass Scan Results

| Camera | Files | Avg PSNR | Outliers (<45 dB) | ANS wins |
|--------|-------|----------|--------------------|----------|
| 45MP 14-bit DSLR | 6,713 | 65.1 dB | 2 (43.5, 43.6 dB) | N/A |
| 100MP 16-bit MF | 260 | 53.8 dB | 0 | 80% (209/260) |

16-bit medium format by ISO:
- ISO 64-200: 100 files, avg savings 12-19%, ANS wins 83/100
- ISO 400-800: 71 files, avg savings -2%, ANS wins 37/71
- ISO 1600+: 91 files, avg savings 7-29%, ANS wins 90/91

### Quality Metrics (45MP 14-bit DSLR, ISO ~200)

| Mode | GPR Size | PSNR | SSIM | Noise σ Ratio |
|------|----------|------|------|---------------|
| VLC | 15.5 MB | 55.61 dB | 0.99984 | 1.10 |
| ANS | 21.5 MB | 55.61 dB | 0.99984 | 1.10 |
| ANS+DN | 21.4 MB | 50.74 dB | 0.99933 | 1.80 |

ANS and VLC produce identical quality at the same quant settings (only entropy coding differs). ANS+DN applies noise-aware quantization which trades 5 dB PSNR for noise-transparent compression.

### Performance (Apple Silicon, single-threaded baseline)

| Operation | 45MP 14-bit | 100MP 16-bit |
|-----------|-----------|-------------|
| Encode (ANS+DN) | 1.1s | 2.5s |
| Decode (ANS+DN) | 0.8s | 1.5s |

Optimizations: parallel ANS pre-encoding (4 threads), precomputed sigma LUT, Irwin-Hall PRNG noise restoration.
