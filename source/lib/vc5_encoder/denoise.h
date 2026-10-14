/*! @file denoise.h
 *
 *  @brief Wavelet-domain noise estimation and soft thresholding for compression.
 *
 *  Reduces entropy in highpass wavelet bands by removing coefficients below
 *  the noise floor.  Uses sensor-calibrated noise parameters (DNG NoiseProfile)
 *  when available, falling back to robust MAD estimation from image data.
 *
 *  References:
 *    Donoho & Johnstone (1994) - "Ideal spatial adaptation by wavelet shrinkage"
 *    Goesele & Heidrich (2001) - "Entropy-Based Dark Frame Subtraction"
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 */

#ifndef DENOISE_H
#define DENOISE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*! Wavelet filter noise gain per subband (10 entries, indexed by flat subband ID) */
extern const double wavelet_noise_gain[10];

/*!
    @brief Denoise all highpass bands of one channel's wavelet transform

    Estimates noise sigma (from calibration or MAD fallback), then applies
    soft thresholding to all 9 highpass subbands (LH/HL/HH × 3 levels).
    The lowpass band is never modified.

    @param transform        Channel's wavelet transform (modified in-place)
    @param strength         User threshold multiplier (0.0–1.0, default 1.0)
    @param noise_scale      DNG NoiseProfile scale (0 = use MAD fallback)
    @param noise_offset     DNG NoiseProfile offset
    @return                 Estimated noise sigma for this channel
*/
double DenoiseTransform(TRANSFORM *transform, double strength,
                        double noise_scale, double noise_offset);

/*!
    @brief Estimate noise sigma from HH band using MAD (fallback)

    Robust estimator: sigma = median(|coefficients|) / 0.6745

    @param data     Band coefficient data
    @param width    Band width in pixels
    @param height   Band height in rows
    @param pitch    Row stride in bytes
    @return         Estimated noise sigma
*/
double EstimateNoiseSigma(const PIXEL *data, DIMENSION width,
                          DIMENSION height, DIMENSION pitch);

/*!
    @brief Compute noise sigma from calibrated Poisson-Gaussian model

    Uses the DNG NoiseProfile parameters with the lowpass band mean
    as the signal level: sigma = sqrt(noise_scale * mean + noise_offset)

    @param lowpass_data     Lowpass band data (signal reference)
    @param width            Band width
    @param height           Band height
    @param pitch            Row stride in bytes
    @param noise_scale      DNG NoiseProfile scale parameter
    @param noise_offset     DNG NoiseProfile offset parameter
    @return                 Calibrated noise sigma
*/
double CalibratedNoiseSigma(const PIXEL *lowpass_data, DIMENSION width,
                            DIMENSION height, DIMENSION pitch,
                            double noise_scale, double noise_offset);

/*!
    @brief Apply soft thresholding to a wavelet band in-place

    |c| <= T  →  0
    |c| >  T  →  sign(c) * (|c| - T)

    @param data         Band data (modified in-place)
    @param width        Band width
    @param height       Band height
    @param pitch        Row stride in bytes
    @param threshold    Absolute threshold value
*/
void SoftThresholdBand(PIXEL *data, DIMENSION width, DIMENSION height,
                       DIMENSION pitch, double threshold);

/*!
    @brief Generate and add back wavelet-domain noise from stored model parameters

    Reconstructs statistically-equivalent noise using a PRNG seeded with
    the stored seed value and the estimated noise sigma per level. This
    restores the original image's noise characteristics after compression.

    @param data         Band data (modified in-place: signal + noise)
    @param width        Band width
    @param height       Band height
    @param pitch        Row stride in bytes
    @param sigma        Noise sigma for this band
    @param seed         PRNG seed (must match the seed stored during encoding)
    @param band_id      Unique band identifier for seed mixing (level*4 + band)
*/
void AddNoiseFromModel(PIXEL *data, DIMENSION width, DIMENSION height,
                       DIMENSION pitch, double sigma, uint32_t seed, int band_id);

/*!
    @brief Noise-aware requantization of wavelet coefficients

    Instead of soft thresholding (binary keep/zero), quantize each
    coefficient to its noise-aware precision. Coefficients below the
    noise floor become zero; coefficients above are rounded to the
    nearest multiple of sigma_noise. This maximizes zero runs (better RLE)
    while bounding error to the noise floor.

    @param data         Band data (modified in-place)
    @param width        Band width
    @param height       Band height
    @param pitch        Row stride in bytes
    @param sigma_noise  Noise sigma for this band
    @param strength     User-controlled strength (0.0-1.0)
*/
void NoiseAwareRequantize(PIXEL *data, DIMENSION width, DIMENSION height,
                          DIMENSION pitch, double sigma_noise, double strength);

/*!
    @brief Estimate noise sigma from raw pixel data (pre-transform)

    Computes local variance across the image to estimate noise level.
    Uses the difference between adjacent pixels as a noise proxy,
    which is robust to image content.

    @param data         Raw pixel data
    @param width        Image width
    @param height       Image height
    @param pitch        Row stride in bytes
    @return             Estimated noise sigma in pixel units
*/
double EstimateRawNoiseSigma(const COMPONENT_VALUE *data, DIMENSION width,
                             DIMENSION height, size_t pitch);

/*!
    @brief Compute noise-aware quantization table

    Given a noise sigma in pixel units, computes the noise sigma in each
    wavelet subband and returns quant divisors that ensure noise is
    quantized away while preserving all signal above the noise floor.

    @param raw_sigma        Noise sigma from EstimateRawNoiseSigma
    @param default_table    Default quant table (quality setting)
    @param output_table     Output table (max of default and noise-based)
    @param table_length     Number of entries in the table
    @param strength         User-controlled denoise strength (0.0-1.0)
*/
void ComputeNoiseAwareQuantTable(double raw_sigma, const int *default_table,
                                 int *output_table, int table_length,
                                 double strength);

/*!
    @brief Apply Generalized Anscombe Transform to a component array in-place

    Stabilizes Poisson-Gaussian noise variance so that the output has
    approximately unit Gaussian noise regardless of signal level.

    GAT: f(x) = (2/α) × sqrt(α×x + 3/8×α² + σ²)

    @param data         Component array data (modified in-place)
    @param width        Array width
    @param height       Array height
    @param pitch        Row stride in bytes
    @param alpha        Poisson noise parameter (DNG noise_scale)
    @param sigma_sq     Gaussian noise variance (DNG noise_offset)
*/
void AnscombeForward(COMPONENT_VALUE *data, DIMENSION width, DIMENSION height,
                     size_t pitch, double alpha, double sigma_sq);

/*!
    @brief Apply exact unbiased inverse Generalized Anscombe Transform

    Inverts the GAT applied during encoding, restoring original signal scale.
    Uses closed-form algebraic inverse (no bias correction for exact round-trip).

    @param data         Component array data (modified in-place)
    @param width        Array width
    @param height       Array height
    @param pitch        Row stride in bytes
    @param alpha        Poisson noise parameter (must match forward transform)
    @param sigma_sq     Gaussian noise variance (must match forward transform)
*/
void AnscombeInverse(COMPONENT_VALUE *data, DIMENSION width, DIMENSION height,
                     size_t pitch, double alpha, double sigma_sq);

#ifdef __cplusplus
}
#endif

#endif /* DENOISE_H */
