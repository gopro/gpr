# Noise-Aware Compression — Complete Pipeline Redesign

## Context

Research into EMVA 1288, Wei et al. (CVPR 2020), Imatest, and commercial noise replacement tools reveals our current implementation covers the most important noise sources but has gaps in the noise taxonomy and pipeline ordering. This plan corrects the architecture to match the physics-based signal chain.

## The Complete Noise Taxonomy

Per EMVA 1288, the canonical noise model is:

```
σ_total² = σ_dark² + σ_shot² × V + σ_PRNU² × V²
```

Our current implementation covers `σ_dark²` (read noise, DSNU via Phase C) and `σ_shot² × V` (photon shot noise via Phase A+B). We're **missing σ_PRNU² × V²** — the gain variation that scales quadratically with signal.

The full signal chain with all noise sources:

```
Photons arrive (Poisson)
  → Photodiode converts to electrons (+ dark current + DSNU)
  → Transfer gate (+ kT/C reset noise)
  → Source follower amplifies (+ 1/f noise + thermal noise + RTS noise)
  → Column readout (+ column FPN + row noise/banding)
  → ADC digitizes (+ quantization noise)
  → Digital output (raw DN value)
```

## Corrected Pipeline Order

### Encoder (subtract noise before compression)

```
Raw Bayer pixels (uint16_t)
  1. Subtract DSNU dark frame          [additive, spatial, per-pixel]
  2. Divide by PRNU gain map           [multiplicative, spatial, per-pixel]  ← NEW
  3. Subtract per-row mean offset      [additive, row-correlated]
  4. Subtract per-column mean offset   [additive, column-correlated]
  5. Apply log curve                   [existing — EncoderLogCurve]
  6. Channel separation (GS/GD/RG/BG) [existing]
  7. Anscombe variance stabilization   [existing — Phase B]
  8. Wavelet transform                 [existing]
  9. Soft threshold (noise removal)    [existing — Phase A]
  10. Quantize + entropy encode        [existing]
```

### Decoder (reconstruct noise after decompression)

```
  1. Entropy decode + dequantize       [existing]
  2. Inverse wavelet transform         [existing]
  3. Noise reconstruction (PRNG)       [existing — Phase A reconstruction]
  4. Inverse Anscombe                  [existing — Phase B inverse]
  5. Component inverse (R/G1/G2/B)     [existing]
  6. Inverse log curve                 [existing — DecoderLogCurve]
  7. Add per-column mean offset        [reverse of encoder step 4]
  8. Add per-row mean offset           [reverse of encoder step 3]
  9. Multiply by PRNU gain map         [reverse of encoder step 2]  ← NEW
  10. Add DSNU dark frame              [reverse of encoder step 1]
```

**Key principle**: Steps 1-4 on the encoder reverse in order as steps 7-10 on the decoder. Last subtracted = first added back.

## What Needs to Change in Code

### 1. Extend noise_model to include PRNU

**File**: `source/lib/vc5_common/noise_model.h`

Add to `fpn_model` struct:
```c
int         has_prnu;                   // PRNU calibration loaded
double      prnu_mean_gain[4];          // Per-channel mean PRNU gain (normalize to 1.0)
double      prnu_poly_coeffs[4][FPN_MAX_POLY_TERMS]; // PRNU polynomial model
double      *prnu_row_gains[4];         // Per-row PRNU gain variation
double      *prnu_col_gains[4];         // Per-column PRNU gain variation
int16_t     *precomputed_prnu_map;      // Precomputed PRNU map (fixed-point 1.16 format)
```

**File**: `source/lib/vc5_common/noise_model.c`

New functions:
```c
void prnu_correct(const fpn_model *model, uint16_t *raw, int width, int height);
void prnu_restore(const fpn_model *model, uint16_t *raw, int width, int height);
```

### 2. Move noise correction INTO UnpackPixel (per-pixel, before log curve)

**Current approach**: `fpn_subtract()` runs on the whole raw buffer in `gpr.cpp` before the encoder.

**New approach**: Pass the noise model into `UnpackPixel_14/16/12` and apply corrections per-pixel, in the correct order, BEFORE the log curve.

**File**: `source/lib/vc5_encoder/raw.c`

Change `UnpackPixel_14` to accept noise model pointer:
```c
static void UnpackPixel_14(uint16_t *input_row1_ptr, uint16_t *input_row2_ptr,
                           int column, int row, PIXEL *output_buffer[], bool rggb,
                           const fpn_model *noise)  // NEW parameter
{
    // Read raw pixels (lines 63-74)
    R1 = input_row1_ptr[2 * column + 0];
    G1 = input_row1_ptr[2 * column + 1];
    G2 = input_row2_ptr[2 * column + 0];
    B1 = input_row2_ptr[2 * column + 1];

    // >>> NEW: Apply noise corrections in order <<<
    if (noise && noise->valid)
    {
        int r = row, c = 2 * column;  // Full Bayer grid coordinates

        // Step 1: DSNU subtraction (dark frame)
        R1 = clamp_sub(R1, noise->precomputed_map[(r)*width + (c)]);
        G1 = clamp_sub(G1, noise->precomputed_map[(r)*width + (c+1)]);
        G2 = clamp_sub(G2, noise->precomputed_map[(r+1)*width + (c)]);
        B1 = clamp_sub(B1, noise->precomputed_map[(r+1)*width + (c+1)]);

        // Step 2: PRNU correction (gain normalization)
        if (noise->has_prnu && noise->precomputed_prnu_map)
        {
            R1 = clamp_div(R1, noise->precomputed_prnu_map[(r)*width + (c)]);
            // ... etc
        }

        // Step 3-4: Row/column noise (already in precomputed_map)
    }

    // Apply log curve (lines 76-80) — unchanged
    R1 = ApplyEncoderCurve(EncoderLogCurve14, R1);
    ...
```

**Why move it here instead of whole-buffer**: The per-pixel approach allows PRNU (multiplicative) to be applied in the correct order relative to DSNU (additive) without requiring two full-buffer passes.

### 3. Mirror corrections in decoder PackComponentsToRAW

**File**: `source/lib/vc5_decoder/raw.c`

After the inverse log curve (line 262), add noise reconstruction:
```c
R  = DecodeLogValue(R, log_bits);
B  = DecodeLogValue(B, log_bits);
G1 = DecodeLogValue(G1, log_bits);
G2 = DecodeLogValue(G2, log_bits);

// >>> NEW: Restore noise in reverse order <<<
if (noise_model && noise_model->valid && !denoise_output)
{
    // Step 9: PRNU restore (multiply by gain)
    // Step 10: DSNU restore (add dark frame)
}
```

### 4. Remove fpn_subtract from gpr.cpp raw buffer path

**File**: `source/lib/gpr_sdk/private/gpr.cpp`

Remove the `fpn_subtract()` call at line 1934. The correction now happens per-pixel inside `UnpackPixel_*`.

Similarly, remove `fpn_add_back()` from `gpr_convert_gpr_to_raw_ex()`. The reconstruction now happens inside `PackComponentsToRAW`.

## Calibration Pipeline Design

### Per-Camera Calibration Protocol

```
CALIBRATION SESSION (automated via calibrate tool):

1. DARK FRAMES (DSNU + Read Noise + Row Noise)
   - Body cap on
   - Shoot N frames (N >= 50) at each ISO: 64, 200, 800, 3200, 12800
   - Exposure times: 1/1000s (read noise only), 1s, 10s, 30s (dark current)
   - Temperature: cold start + after 30 min warm-up
   → Produces: dsnu_map[iso][temp], row_offsets[iso], read_noise_sigma[iso]

2. FLAT FIELD FRAMES (PRNU)
   - Evenly illuminated white surface (light box or overcast sky)
   - Deliberately defocused (avoids texture)
   - Shoot N frames (N >= 50) at each ISO: 64, 200, 800
   - Exposure: mid-range (avoid clipping and dark current dominance)
   → Produces: prnu_gain_map[iso], col_offsets

3. NOISE MODEL FITTING
   - Fit DSNU: polynomial + row/col offsets + residual sigma
   - Fit PRNU: polynomial + row/col gains + residual sigma
   - Compute: noise_scale and noise_offset per ISO (Poisson-Gaussian params)
   - Temperature model: fit dark current vs temperature coefficient

4. OUTPUT: sensor_calibration.json
   {
     "sensor": "HERO10 Black",
     "serial": "C1234...",
     "calibration_date": "2026-04-18",
     "dark_frames": { ... per-ISO DSNU model ... },
     "flat_field": { ... per-ISO PRNU model ... },
     "noise_params": { ... per-ISO noise_scale/offset ... },
     "thermal": { ... temperature coefficients ... }
   }
```

### Automated Calibration Tool

**New**: `source/app/calibrate/calibrate.c`

```
Usage: calibrate --dark-dir <path> --flat-dir <path> --output <sensor.json>

Workflow:
  1. Scan dark directory: group frames by ISO and exposure time
  2. For each group: average frames → DSNU map, compute row/col offsets
  3. Scan flat directory: group by ISO
  4. For each group: average frames, divide by mean → PRNU gain map
  5. Fit polynomial models to DSNU and PRNU maps
  6. Compute noise_scale/offset per ISO from photon transfer curve
  7. Output comprehensive JSON calibration file
```

The calibrate tool reuses existing infrastructure:
- `fpn_extract.c` frame averaging logic
- `analyze_raw.c` noise estimation (MAD, PTC fitting)
- `noise_model.c` polynomial fitting

### Integration with gpr_tools

```
# Calibrate sensor (one-time per camera)
calibrate --dark-dir ./dark_frames/ --flat-dir ./flat_fields/ -o hero10.json

# Encode with full sensor model
gpr_tools -i photo.RAW -o photo.GPR --Denoise -N 10 -F hero10.json

# Decode with noise reconstruction (default: transparent)
gpr_tools -i photo.GPR -o photo.RAW

# Decode with denoising (noise NOT reconstructed)
gpr_tools -i photo.GPR -o photo.RAW --DenoiseOutput
```

## Implementation Stages

### Stage 1: Restructure pipeline (code only, no new data)
1. Move noise correction from `gpr.cpp` whole-buffer into `UnpackPixel_*` per-pixel
2. Add PRNU fields to `fpn_model` struct
3. Add `prnu_correct()` / `prnu_restore()` to `noise_model.c`
4. Mirror in decoder `PackComponentsToRAW`
5. Pass noise model through encoder/decoder parameter chain

### Stage 2: Build calibration tool (code only)
1. Create `source/app/calibrate/calibrate.c`
2. Combine logic from `fpn_extract.c` (averaging) and `analyze_raw.c` (noise estimation)
3. Add flat-field processing (divide by mean → PRNU map)
4. Output comprehensive JSON with DSNU + PRNU + per-ISO noise params

### Stage 3: Calibrate sensors (requires shooting)
1. Shoot dark frames: body cap, multiple ISOs, multiple exposures
2. Shoot flat fields: light box or overcast sky, multiple ISOs
3. Run calibrate tool
4. Test with calibrated encode/decode

### Stage 4: Validate and measure
1. Compare compression with full calibration vs Phase A alone
2. Measure PSNR of noise-transparent round-trip
3. Verify PRNU correction improves compression in bright regions
4. Verify row noise correction eliminates banding artifacts

## Files to Modify

| File | Change |
|------|--------|
| `source/lib/vc5_common/noise_model.h` | Add PRNU fields to fpn_model |
| `source/lib/vc5_common/noise_model.c` | Add prnu_correct/restore, update precompute |
| `source/lib/vc5_encoder/raw.c` | Pass noise model to UnpackPixel_*, apply per-pixel corrections |
| `source/lib/vc5_decoder/raw.c` | Add noise reconstruction in PackComponentsToRAW |
| `source/lib/gpr_sdk/private/gpr.cpp` | Remove whole-buffer fpn_subtract, pass model to encoder |
| `source/lib/vc5_encoder/encoder.c` | Pass noise model through to raw unpacking |
| `source/lib/vc5_encoder/vc5_encoder.h` | Add noise model to encoder parameters |
| New: `source/app/calibrate/calibrate.c` | Automated calibration tool |

## Verification

1. Without calibration: identical to current behavior (79.34 dB, no regression)
2. With DSNU-only calibration: matches current FPN results
3. With DSNU+PRNU calibration: improved compression in bright regions
4. Full calibration + denoise: best compression ratios across all ISOs
5. All existing sample GPR files still decode correctly
