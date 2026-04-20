# Noise-Aware Compression for GPR — Complete Noise Model & Implementation Plan

## The Sensor Noise Model

A raw image sensor produces signal corrupted by multiple independent noise sources operating at different layers of the imaging pipeline. Each source has distinct statistical properties and requires a different mitigation strategy.

### Layer 1: Fixed-Pattern Noise (deterministic, sensor-specific)

**DSNU** (Dark Signal Non-Uniformity) — spatial pattern of dark current that varies per-pixel due to manufacturing defects. Each pixel has a slightly different baseline dark current rate. This pattern is:
- Deterministic per sensor (same every frame)
- Temperature-dependent (dark current doubles per ~6°C)
- Reproducible from a sensor ID + temperature + exposure time

**PRNU** (Photo Response Non-Uniformity) — per-pixel gain variation. Each pixel converts photons to electrons at a slightly different rate. This is:
- Deterministic per sensor
- Signal-dependent (multiplicative, not additive)
- Stable over the sensor's lifetime

**Thermal gradients** — ADC and readout circuitry generate heat that creates spatially-varying temperature across the sensor. This causes:
- Spatially-correlated dark current that's higher near heat sources (ADCs, readout amplifiers)
- A smooth, low-frequency spatial pattern (well-modeled by low-order basis functions)
- Dependence on usage pattern (warm-up time, burst shooting, video recording)
- Orthogonal to wavelet-band noise — this is a pixel-domain spatial effect

### Layer 2: Photon Shot Noise (stochastic, signal-dependent)

The fundamental quantum noise from photon arrival statistics. Follows a Poisson distribution: variance equals the signal level. This is:
- Signal-dependent (brighter regions have more noise in absolute terms)
- Not removable without losing signal — but its entropy can be made uniform via variance stabilization
- Characterized by the sensor's gain constant (electrons per DN)

### Layer 3: Read Noise (stochastic, signal-independent)

Gaussian noise from the analog readout chain (source follower, column amplifiers, ADC). This is:
- Additive, approximately Gaussian
- Constant across the frame (same σ everywhere)
- Characterized per ISO setting

### Combined Model: Poisson-Gaussian with FPN

For pixel value `y` at position `(i,j)`:

```
y(i,j) = [gain(i,j) × photons(i,j)] + dark(i,j,T) + thermal(i,j,T,t) + read_noise

where:
  gain(i,j)         = 1 + prnu(i,j)           — PRNU (Phase C)
  dark(i,j,T)       = dsnu(i,j) × exp(T/Td)   — DSNU (Phase C)
  thermal(i,j,T,t)  = Σ ak × basis_k(i,j)     — Thermal gradient (Phase C)
  photons(i,j)      ~ Poisson(signal)           — Shot noise (Phase B)
  read_noise         ~ N(0, σ_read²)            — Read noise (Phase A/B)
```

The DNG NoiseProfile gives us the combined Poisson-Gaussian parameters:
- `noise_scale` ≈ 1/gain (maps photon noise to DN noise)
- `noise_offset` ≈ σ_read² (read noise variance)

## Measured Sensor Characteristics

From analysis of actual images (no formal calibration):

| Sensor | Resolution | ISO | DNG noise_scale | Read Noise | Dark σ | FPN Direction | Entropy |
|--------|-----------|-----|-----------------|-----------|--------|---------------|---------|
| Hero5 Black | 4000×3000 | 100 | 7.81e-05 | 57.7 DN | 22.9 DN | Horizontal | 10.90 bits |
| Hero6 Black | 4000×3000 | 100 | 1.56e-04 | 39.8 DN | 22.9 DN | Horizontal | 10.98 bits |
| Hero7 Black | 4000×3000 | 100 | 3.94e-04 | 51.4 DN | 13.1 DN | Horizontal | 10.74 bits |
| Hero9 Black | 5568×4176 | 100 | 1.56e-04 | 46.1 DN | 26.0 DN | Horizontal | 11.17 bits |
| Fusion | 3104×3000 | 100 | 7.81e-05 | 35.6 DN | 11.1 DN | Horizontal | 10.85 bits |
| X2D 100C | 11664×8750 | 64 | 2.10e-05 | 252 DN | 128 DN | Vertical | 13.63 bits |
| X2D 100C | 11664×8750 | 200 | 6.75e-05 | 222 DN | 130 DN | Vertical | 14.01 bits |
| X2D 100C | 11664×8750 | 12800 | 4.27e-03 | 915 DN | 278 DN | Vertical | 13.71 bits |

### X2D Dark Frame Calibration (50 frames per ISO, Phocus tethered capture)

| ISO | Black Level | DSNU σ R | DSNU σ Gr | DSNU σ Gb | DSNU σ B |
|-----|------------|---------|----------|----------|---------|
| 64 | 4094.2 | 0.60 | 0.65 | 0.85 | 0.86 |
| 200 | 4094.5 | 0.96 | 1.01 | 1.33 | 1.29 |
| 12800 | 4089.9 | 56.17 | 57.27 | 80.48 | 81.84 |

**Key finding**: The X2D's BSI sensor has exceptionally low DSNU (sub-1.3 DN at ISO 200).
Even at ISO 12800, DSNU (56-82 DN) is small relative to total noise (~915 DN).
Dark frame calibration adds negligible compression improvement over Phase A wavelet
thresholding alone — Phase A already captures pattern noise in the wavelet domain.

**Key observations:**
- GoPro sensors: consistent horizontal banding (FPN from row readout)
- X2D: vertical banding (FPN from column ADCs)
- X2D DNG NoiseProfile scales correctly with ISO (3.2x for 64→200)
- Dark sigma is the noise floor in shadow regions (read noise + dark current)
- Shannon entropy: 10.7–14.0 bits/pixel (theoretical lossless compression limit: 1.1–1.3x)

### HERO10 Dark Frame Calibration (50 frames per ISO, manual capture)

| ISO | Black Level | DSNU σ R | DSNU σ Gr | DSNU σ Gb | DSNU σ B |
|-----|------------|---------|----------|----------|---------|
| 100 | 12.0 | 0.41 | 0.29 | 0.32 | 0.38 |
| 400 | 12.0 | 1.56 | 1.05 | 1.19 | 1.45 |
| 1600 | 14.1 | 4.32 | 3.37 | 3.73 | 4.03 |

**Key finding**: HERO10 DSNU is even lower than the X2D at comparable ISOs. DSNU scales
linearly with ISO (as expected from analog gain amplifying fixed-pattern offsets). At ISO 1600,
DSNU (~4 DN) is still only 25% of the read noise (~16 DN from PTC data).

### Photon Transfer Curve (HERO10 Black, sunset timelapse ramp)

From 1,408-frame sunset-to-dark ramp (ISO 100→800, 2 hours):

| ISO | Read Noise (DN) | Dark σ (DN) | Entropy (bits) |
|-----|-----------------|-----------|----------------|
| 100 | 6.3 | 4.5 | 8.41 |
| 131 | 7.3 | 4.7 | 8.47 |
| 230 | 9.4 | 5.0 | 8.54 |
| 400 | 6.3–9.4 | 2.2–2.8 | 8.08–8.34 |
| 474 | 10.5 | 3.0 | 8.46 |
| 787 | 15.7 | 3.8 | 8.69 |
| 800 | 4.2–8.4 | 3.6–4.2 | 4.52–7.69 |

**Key findings:**
- Read noise scales ~sqrt(ISO): 6.3 DN at ISO 100 → 15.7 DN at ISO 787
- Dark sigma drifts upward over time: 2.2→4.2 DN (thermal warm-up over 2 hours)
- Entropy drops from 8.4 to 4.5 bits as scene darkens (noise becomes dominant)

## Three-Phase Implementation

### Phase A: Wavelet-Domain Soft Thresholding ✅ DONE

**Status**: Implemented and merged. Operates on quantized wavelet coefficients after the transform.

**What it addresses**: Combined noise floor in the wavelet domain. Uses DNG NoiseProfile for calibrated sigma or MAD fallback. Reduces entropy by zeroing sub-threshold highpass coefficients.

**Results**: 2-60% compression improvement depending on strength. Default 100% is aggressive; 10-30% is conservative.

### Phase B: Generalized Anscombe Variance Stabilization

**What it addresses**: Signal-dependent photon shot noise. The Poisson component means noise variance varies with signal level — bright regions have more noise than dark regions. This makes wavelet thresholding suboptimal because a single threshold is too aggressive for dark regions and too weak for bright regions.

**How**: Apply the Generalized Anscombe Transform (GAT) to component arrays AFTER unpacking (log curve + channel separation) but BEFORE the wavelet transform. This converts the Poisson-Gaussian noise into approximately uniform Gaussian noise, making the Phase A wavelet thresholding uniformly effective across all signal levels.

**GAT forward**: `f(x) = 2/α × sqrt(α×x + 3/8×α² + σ²)` where α = noise_scale, σ² = noise_offset
**GAT inverse** (exact unbiased): Lookup table or closed-form approximation

**Insertion point (encoder)**: 
- `encoder.c` between `ImageUnpackingProcess()` (line 356) and `EncodingProcess()` (line 372)
- Apply GAT to each component array in `unpacked_image.component_array_list[0-3]`

**Insertion point (decoder)**:
- After `ReconstructUnpackedImage()` in `decoder.c`, before `PackComponentsToRAW()`
- Apply inverse GAT to each component array

**Parameters**: Uses existing `noise_scale` and `noise_offset` from DNG NoiseProfile. No new metadata needed.

**Files to modify**:
- `source/lib/vc5_encoder/denoise.c` — add `AnscombeForward()`, `AnscombeInverse()`, `BuildAnscombeInverseLUT()`
- `source/lib/vc5_encoder/denoise.h` — declare new functions
- `source/lib/vc5_encoder/encoder.c` — call AnscombeForward on component arrays before wavelet transform
- `source/lib/vc5_decoder/decoder.c` — call AnscombeInverse on component arrays after inverse transform
- `source/lib/vc5_decoder/CMakeLists.txt` — link denoise module (or create shared noise module)
- `source/lib/gpr_sdk/public/gpr_tuning_info.h` — add `variance_stabilize` flag

**Key consideration**: The log curve already compresses the dynamic range somewhat (like a square root). The Anscombe transform applied AFTER the log curve needs to account for the log curve's effect on the noise statistics. The combined transform is: `GAT(LogCurve(x))` which is more complex than bare GAT. Phase B should compute the composite noise variance after the log curve and apply a modified stabilization.

### Phase C: Fixed-Pattern Noise Model with PRNG Seed

**What it addresses**: Deterministic, sensor-specific noise patterns (DSNU, PRNU, thermal gradients). These are not random — they're predictable from sensor calibration data and operating conditions. Since they're deterministic, they can be perfectly subtracted (unlike shot noise which is truly random).

**How**: Before any processing (before the log curve, in the raw Bayer domain), subtract the FPN estimate from the raw pixels. Store compact model parameters in metadata so the decoder can reconstruct and add back the FPN if desired.

**Noise model components**:

1. **DSNU (per-pixel dark offset)**: Characterized from dark frames at known temperature. Stored as a compact model:
   - Mean dark level (scalar per channel)
   - Spatial basis coefficients (DCT or polynomial, ~100 coefficients captures most structure)
   - PRNG seed + distribution parameters for the stochastic residual after basis subtraction
   - Temperature coefficient for scaling

2. **PRNU (per-pixel gain)**: Characterized from flat-field frames. Stored as:
   - Mean gain (scalar per channel)
   - Spatial gain variation map (low-rank approximation or polynomial)
   - Very stable — can be a one-time calibration per sensor

3. **Thermal gradient**: Modeled as a smooth spatial function:
   - Low-order 2D polynomial coefficients (e.g., 6 coefficients for quadratic: a + bx + cy + dx² + exy + fy²)
   - Temperature-dependent scaling factor
   - Estimated from sensor metadata (temperature tag if available) or calibration lookup

**Insertion point (encoder)**:
- Inside `UnpackPixel_14()` / `UnpackPixel_16()` in `raw.c`, BEFORE the log curve application
- After extracting R1, G1, G2, B1 from input (line ~74), subtract FPN: `R1 -= fpn_estimate(row, col, channel, temperature)`

**Insertion point (decoder)**:
- After `PackComponentsToRAW()` in `raw.c` (decoder), add FPN back if requested
- Or: leave it subtracted (the decoded image is "cleaner" without FPN)

**Metadata storage**:
- New `gpr_noise_model` struct in `gpr_tuning_info`:
  ```
  uint32_t  fpn_seed;              // PRNG seed for stochastic FPN residual
  double    fpn_dsnu_coeffs[64];   // Spatial basis coefficients for DSNU
  double    fpn_prnu_coeffs[64];   // Spatial basis coefficients for PRNU  
  double    thermal_poly[6];       // 2D polynomial for thermal gradient
  double    sensor_temperature;    // Temperature at capture time
  int       fpn_model_valid;       // Flag: calibration data available
  ```
- Stored in DNG via custom XMP metadata tags
- Total metadata overhead: ~1KB (vs megabytes for per-pixel maps)

**Files to modify**:
- New: `source/lib/vc5_common/noise_model.c` / `noise_model.h` — FPN model evaluation, basis functions, PRNG
- `source/lib/vc5_encoder/raw.c` — subtract FPN in UnpackPixel functions
- `source/lib/vc5_decoder/raw.c` — optionally add FPN back in PackComponents
- `source/lib/gpr_sdk/public/gpr_tuning_info.h` — add noise model struct
- `source/lib/gpr_sdk/private/gpr.cpp` — serialize/deserialize noise model to DNG XMP
- CLI: `--fpn-calibration <file>` to load sensor calibration data

**Calibration workflow** (external to this codebase):
1. Capture dark frames at multiple temperatures → fit DSNU model
2. Capture flat-field frames → fit PRNU model
3. Capture thermal gradient calibration sequence → fit polynomial
4. Export to calibration file (JSON with model coefficients + seed)
5. Pass calibration file to encoder via CLI flag

## Implementation Order

### Phase B Commits (on feature/noise-aware-compression branch):

1. **Add Anscombe forward/inverse to denoise module** — `AnscombeForward()` operates on component arrays in-place, `AnscombeInverse()` with exact unbiased inverse via LUT
2. **Insert variance stabilization in encoder** — between unpacking and wavelet transform, guarded by `variance_stabilize` flag
3. **Insert inverse stabilization in decoder** — after inverse transform, before component packing
4. **Add CLI flag** — `--stabilize` to enable variance stabilization
5. **Test**: Verify round-trip, measure PSNR improvement of Phase A thresholding with stabilized data

### Phase C Commits (new branch feature/fpn-model):

1. **Create noise_model module** — basis function evaluation, PRNG-seeded noise generation, polynomial thermal model
2. **Add FPN subtraction to encoder raw unpacking** — subtract before log curve
3. **Add metadata serialization** — store model params in DNG XMP
4. **Add calibration file loader** — parse sensor calibration JSON
5. **Add optional FPN reconstruction in decoder** — for applications that want the original noise characteristics
6. **Test**: Verify with synthetic calibration data, measure entropy reduction

## Verification Strategy

For each phase:

1. **Compression ratio**: Compare file sizes with and without the feature
2. **PSNR**: Measure quality loss (should be minimal — we're removing noise, not signal)
3. **Visual quality**: Decode and inspect for artifacts (banding, over-smoothing, color shifts)
4. **Round-trip correctness**: Encode→decode without the feature still works identically
5. **Backwards compatibility**: Old files decode correctly, default behavior unchanged
6. **16-bit data**: Test with X2D to verify the wider dynamic range benefits

## Measured Compression Results

### Improvement Scales with Noise Level

| Sensor | ISO | Normal Q4 | Denoise N=30 Q4 | Improvement Factor |
|--------|-----|-----------|-----------------|-------------------|
| X2D 100C | 64 | 3.6x | 4.7x | 1.3x |
| Hero6 Black | 100 | 4.2x | 5.9x | 1.4x |
| Hero10 Black (night TL) | 400 | 3.3x | 10.8x | 3.3x |
| Hero10 Black | 1600 | 3.5x | 11.5x | 3.3x |
| X2D 100C | 12800 | 2.2x | 10.1x | 4.6x |

At high ISO, noise dominates signal entropy. The noise-aware compression identifies this noise entropy and separates it, achieving up to 3.3x better compression. At low ISO (clean sensor data), the improvement is modest (1.3-1.4x) because there's less noise to separate.

### Dark Frame Performance

On a near-dark timelapse frame (HERO10, 30s, ISO 400, 83% dark pixels):
- Normal Q4: 9.1 MB (3.3x compression)
- Denoise N=30: 2.8 MB (10.8x compression)
- **69% file size reduction** from noise separation alone

## Key Design Principles

1. **Sensor characterization first** — always prefer calibrated parameters over blind estimation
2. **Layer independence** — each noise source is addressed independently at the right pipeline stage
3. **Compact metadata** — store model parameters (hundreds of bytes), not per-pixel maps (megabytes)
4. **Optional and backwards-compatible** — every feature disabled by default, no bitstream changes
5. **Thermal noise is orthogonal** — treated as a separate spatial model from wavelet-band noise, with its own temperature-dependent parameters
