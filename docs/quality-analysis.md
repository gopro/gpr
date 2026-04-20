# GPR Quality Analysis — 16-Bit

Test image: Hasselblad X2D 100C, 11664x8750, 16-bit Bayer (194.7 MB raw)
Source: `2024_12_Dec_Austin_0186.dng` (138.2 MB lossless DNG)

## File Size

| Q | GPR Size | Compression | vs DNG | Space Saving |
|---|----------|-------------|--------|--------------|
| 0 | 21.4 MB | 9.1x | 6.4x smaller | 84.5% |
| 1 | 28.7 MB | 6.8x | 4.8x smaller | 79.2% |
| 2 | 39.2 MB | 5.0x | 3.5x smaller | 71.6% |
| 3 | 46.0 MB | 4.2x | 3.0x smaller | 66.7% |
| 4 | 53.0 MB | 3.7x | 2.6x smaller | 61.6% |
| 5 | 66.9 MB | 2.9x | 2.1x smaller | 51.6% |
| 6 | 89.8 MB | 2.2x | 1.5x smaller | 35.0% |
| 7 | 90.9 MB | 2.1x | 1.5x smaller | 34.2% |
| 8 | 92.0 MB | 2.1x | 1.5x smaller | 33.4% |

Compression ratio is vs uncompressed 16-bit raw (194.7 MB).

## Raw Bayer Domain (pre-demosaic)

This is the domain RAW editors (Lightroom, Capture One, etc.) operate in.
Errors here directly affect all downstream edits.

| Q | PSNR | RMS Error | MAE | MAE % | Max Error | ENOB |
|---|------|-----------|-----|-------|-----------|------|
| 0 | 40.0 dB | 655 DN | 334 DN | 0.509% | 11454 DN | 6.6 bits |
| 1 | 44.6 dB | 386 DN | 209 DN | 0.319% | 6682 DN | 7.4 bits |
| 2 | 48.5 dB | 246 DN | 136 DN | 0.208% | 4437 DN | 8.1 bits |
| 3 | 50.0 dB | 207 DN | 116 DN | 0.176% | 4115 DN | 8.3 bits |
| 4 | 51.2 dB | 181 DN | 100 DN | 0.153% | 4179 DN | 8.5 bits |
| 5 | 52.1 dB | 163 DN | 89 DN | 0.135% | 3749 DN | 8.7 bits |
| 6 | 56.5 dB | 98 DN | 53 DN | 0.081% | 2190 DN | 9.4 bits |
| 7 | 58.6 dB | 77 DN | 41 DN | 0.062% | 1597 DN | 9.7 bits |
| 8 | 58.9 dB | 74 DN | 39 DN | 0.060% | 1566 DN | 9.8 bits |

- ENOB = Effective Number of Bits = PSNR / 6.02
- DN = Digital Number (out of 65535 for 16-bit)
- This sensor captures ~14 bits of real data at ISO 100

## Rendered RGB (16-bit sRGB linear)

After demosaicing + white balance + color matrix. This is what you see on screen.

| Q | PSNR | R PSNR | G PSNR | B PSNR | MAE | >0.5% | >1% |
|---|------|--------|--------|--------|-----|-------|-----|
| 0 | 36.9 dB | 35.2 dB | 39.6 dB | 37.1 dB | 468 DN | 36.8% | 20.4% |
| 1 | 40.5 dB | 38.7 dB | 43.4 dB | 40.7 dB | 317 DN | 28.7% | 13.0% |
| 2 | 43.5 dB | 41.6 dB | 46.4 dB | 43.7 dB | 224 DN | 20.4% | 7.4% |
| 3 | 44.5 dB | 42.6 dB | 47.5 dB | 44.7 dB | 196 DN | 17.2% | 5.8% |
| 4 | 45.4 dB | 43.4 dB | 48.5 dB | 45.6 dB | 176 DN | 14.9% | 4.7% |
| 5 | 46.1 dB | 44.1 dB | 49.4 dB | 46.4 dB | 160 DN | 13.1% | 3.9% |
| 6 | 49.5 dB | 47.7 dB | 52.2 dB | 49.7 dB | 95 DN | 5.4% | 1.2% |
| 7 | 50.9 dB | 49.3 dB | 53.2 dB | 51.2 dB | 72 DN | 3.2% | 0.7% |
| 8 | 51.1 dB | 49.5 dB | 53.4 dB | 51.4 dB | 69 DN | 3.0% | 0.6% |

- Red channel is weakest (lowest PSNR) — typical for Bayer demosaic
- ">0.5%" / ">1%" = % of pixel values with error exceeding that fraction of full scale

## Practical Editing Guidance

Context: This sensor (X2D 100C) captures ~14 bits of real data at ISO 100.
Compression ENOB ranges from 6.6 bits (Q0) to 9.8 bits (Q8).
Compression error is always above sensor noise, so it is the limiting factor.

### Q0 — Aggressive Compression (9.1x, 21 MB)

Raw ENOB: 6.6 bits | PSNR: 40.0 dB | RMS error: 655 DN

- OK: Export to JPEG/web at 4K or smaller — artifacts masked by downsampling + 8-bit
- OK: Basic white balance and exposure +/- 0.5 stop
- OK: Light color grading (LUT application, saturation +/- 20%)
- NO: Shadow recovery — pushing shadows reveals quantization banding
- NO: Heavy curves / local contrast — amplifies per-band wavelet artifacts
- NO: Large prints from full resolution — artifacts visible at ~200 PPI+

Best for: Preview/proxy files, web gallery, social media, quick turnaround

### Q1 — High Compression (6.8x, 29 MB)

Raw ENOB: 7.4 bits | PSNR: 44.6 dB | RMS error: 386 DN

- OK: Exposure adjustment +/- 1 stop
- OK: Moderate white balance correction
- OK: Saturation and vibrance adjustments
- MAYBE: Shadow push up to +1 stop — minor artifacts in deep shadows
- NO: Heavy shadow recovery (more than +1 stop)

Best for: Event photography, documentary, editorial with light edits

### Q2-Q3 — Moderate Compression (5.0-4.2x, 39-46 MB)

Raw ENOB: 8.1-8.3 bits | PSNR: 48.5-50.0 dB | RMS error: 207-246 DN

- OK: Exposure +/- 1.5 stops
- OK: Moderate shadow recovery (+1.5 stops)
- OK: Color grading and creative white balance shifts
- OK: Clarity / texture / dehaze adjustments
- OK: Print up to 24x36" at 300 PPI
- MAYBE: Aggressive curves — some wavelet ringing in smooth gradients
- NO: Extreme shadow push (> +2 stops)

Best for: Professional editorial, portrait, landscape with moderate edits

### Q4-Q5 — Balanced (3.7-2.9x, 53-67 MB)

Raw ENOB: 8.5-8.7 bits | PSNR: 51.2-52.1 dB | RMS error: 163-181 DN

- OK: Exposure +/- 2 stops
- OK: Shadow recovery up to +2 stops
- OK: Aggressive color grading (split toning, cross-process, film emulation)
- OK: Local adjustments (dodge/burn, graduated filters, radial masks)
- OK: Sharpening and noise reduction
- OK: Large prints at full resolution
- MAYBE: Extreme shadow push (+3 stops) — faint banding in deepest shadows

Best for: Professional photography with full editing workflow

### Q6-Q8 — Near-Lossless (2.2-2.1x, 90-92 MB)

Raw ENOB: 9.4-9.8 bits | PSNR: 56.5-58.9 dB | RMS error: 74-98 DN

- OK: Shadow recovery up to +3 stops
- OK: Extreme exposure adjustments (+/- 3 stops)
- OK: Aggressive local contrast / clarity / HDR tone mapping
- OK: Fine art printing at maximum size
- OK: Pixel-level compositing and retouching
- MAYBE: Extreme shadow push (+4 stops) — may show faint artifacts

Note: Q7 and Q8 are nearly identical (58.6 vs 58.9 dB) because the
quantization divisor hits the noise floor. Q6 offers the best
quality/size tradeoff in this tier.

Best for: Archival, fine art, maximum editing flexibility

## Key Insight

This is a 16-bit sensor with ~14 bits of real data. Even Q8 (best quality)
only preserves ~10 bits of the 14 available. The remaining ~4 bits of sensor
precision are lost to wavelet quantization. For truly lossless archival,
keep the original DNG.

However: for practical photography, 10 bits of ENOB in the raw domain
translates to 8+ bits of final output precision after demosaic + color
processing — more than sufficient for any 8-bit display or print.

The compression artifacts are in the wavelet domain (smooth spatial patterns
across 8x8 or 16x16 pixel blocks), not in the pixel domain. They are less
visually objectionable than JPEG artifacts at the same PSNR.

## Per-Channel Quantization Scaling

VC5 encodes Bayer data in color-difference space (not raw RGGB):

| Channel | Name | Content |
|---------|------|---------|
| 0 (GS) | Green Sum | (Gr+Gb)/2 — luminance |
| 1 (RG) | Red-Green | R-G — chrominance |
| 2 (BG) | Blue-Green | B-G — chrominance |
| 3 (GD) | Green Diff | (Gr-Gb)/2 — detail/noise |

The `--ChannelScale` flag (or `channel_quant_scale` API) applies per-channel
multipliers to quantization. Scale > 1.0 = more compression, < 1.0 = less.

### Test Results (Q4 base, 100MP 16-bit)

| Config | Scale (GS,RG,BG,GD) | Size | Raw PSNR | RGB PSNR | G PSNR | R PSNR | B PSNR |
|--------|---------------------|------|----------|----------|--------|--------|--------|
| Uniform Q4 | 1,1,1,1 | 53.0 MB | 51.2 dB | 44.6 dB | 49.2 dB | 42.0 dB | 45.3 dB |
| A | 1.0, 2.0, 2.0, 3.0 | 39.9 MB | 46.2 dB | 39.9 dB | 45.0 dB | 37.2 dB | 40.7 dB |
| B | 1.0, 3.0, 3.0, 4.0 | 35.3 MB | 43.7 dB | 37.4 dB | 43.2 dB | 34.5 dB | 38.4 dB |
| C | 0.5, 2.0, 2.0, 2.0 | 47.4 MB | 47.5 dB | 40.4 dB | 46.3 dB | 37.5 dB | 41.4 dB |
| D | 1.0, 4.0, 4.0, 6.0 | 31.9 MB | 41.3 dB | 35.5 dB | 41.2 dB | 32.6 dB | 36.5 dB |

### Size-Matched Comparison (per-channel vs uniform)

| Per-Channel | Size | RGB PSNR | Uniform Match | Size | RGB PSNR | Delta |
|-------------|------|----------|---------------|------|----------|-------|
| Q4+A(1,2,2,3) | 39.9 MB | 39.9 dB | Uniform Q2 | 39.2 MB | 42.8 dB | -2.9 dB |
| Q4+C(.5,2,2,2) | 47.4 MB | 40.4 dB | Uniform Q3 | 46.0 MB | 43.8 dB | -3.4 dB |

### Analysis

At the same file size, uniform quality presets outperform per-channel scaling
by 2-3 dB. The reason: VC5's quality tables are already carefully tuned for
the color-difference space. Each preset has per-band ratios (e.g., lower Q for
low-frequency, higher Q for high-frequency) that are perceptually meaningful.
A uniform multiplier across all bands within a channel is less efficient than
using a different preset designed as a coherent whole.

### When Per-Channel Scaling IS Useful

- **Chroma-insensitive workflows**: Machine vision, scientific imaging where
  only luminance matters — compress RG/BG/GD aggressively
- **Noise-dominated detail channel**: At high ISO, GD is mostly noise —
  compressing it with scale 1.5-2.0 saves space with no visible loss
- **Fine-tuning between presets**: When Q3 is too large and Q2 is too lossy,
  `Q3 -c "1.0,1.0,1.0,1.3"` gives a size between Q2 and Q3
- **Artistic intent**: Deliberately softening color detail while preserving
  luminance sharpness

### CLI Usage

```
# Compress chroma 2x, detail 3x (GS untouched)
gpr_tools -i input.dng -o output.gpr -q 4 -c "1.0,2.0,2.0,3.0"

# Boost luminance quality, compress chroma mildly
gpr_tools -i input.dng -o output.gpr -q 3 -c "0.7,1.0,1.0,1.3"
```

## Recommended Presets

| Use Case | Recommended | Size (100MP) | PSNR |
|----------|-------------|-------------|------|
| Proxy / preview / web gallery | Q0 | 21 MB | 40.0 dB |
| Event / documentary (light edit) | Q1 | 29 MB | 44.6 dB |
| Editorial / portrait | Q2 or Q3 | 39-46 MB | 48-50 dB |
| Professional (full edit workflow) | Q4 or Q5 | 53-67 MB | 51-52 dB |
| Archival / fine art | Q6 | 90 MB | 56.5 dB |
| Maximum quality (diminishing returns) | Q7 or Q8 | 91-92 MB | 59 dB |
| Truly lossless | Original DNG | 138 MB | infinite |
