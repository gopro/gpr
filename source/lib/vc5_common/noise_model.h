/*! @file noise_model.h
 *
 *  @brief Fixed-pattern noise model for sensor calibration.
 *
 *  Evaluates a 2D polynomial FPN model per Bayer channel, with optional
 *  PRNG-seeded residual for stochastic FPN components.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#ifndef NOISE_MODEL_H
#define NOISE_MODEL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FPN_MAX_POLY_ORDER 4
#define FPN_MAX_POLY_TERMS 15   /* (4+1)*(4+2)/2 */

/*! Maximum row/column offsets (half resolution for Bayer) */
#define FPN_MAX_ROWS 4400   /* Max sensor height / 2 */
#define FPN_MAX_COLS 6000   /* Max sensor width / 2 */

/*! @brief Per-sensor FPN calibration model */
typedef struct {
    int         valid;                                  /* Model loaded and ready */
    int         width;                                  /* Sensor width in pixels */
    int         height;                                 /* Sensor height in pixels */
    int         poly_order;                             /* Polynomial order (default 4) */
    double      channel_means[4];                       /* Per-channel mean dark level */
    double      poly_coeffs[4][FPN_MAX_POLY_TERMS];    /* Polynomial coefficients per Bayer channel */
    double      residual_sigma[4];                      /* Residual noise sigma per channel */
    uint32_t    seed;                                   /* PRNG seed for residual reconstruction */
    int         has_row_col_offsets;                     /* Row/column offset data loaded */
    int         half_rows;                              /* Number of row offsets per channel */
    int         half_cols;                              /* Number of column offsets per channel */
    double      *row_offsets[4];                        /* Per-row mean offset per Bayer channel (heap allocated) */
    double      *col_offsets[4];                        /* Per-column mean offset per Bayer channel (heap allocated) */
    int16_t     *precomputed_map;                       /* Precomputed DSNU map (full resolution, heap allocated) */

    /* PRNU (Photo Response Non-Uniformity) — multiplicative gain variation */
    int         has_prnu;                               /* PRNU calibration data loaded */
    double      prnu_mean_gain[4];                      /* Per-channel mean PRNU gain (normalized to 1.0) */
    double      prnu_poly_coeffs[4][FPN_MAX_POLY_TERMS]; /* PRNU 2D polynomial per channel */
    double      *prnu_row_gains[4];                     /* Per-row PRNU gain variation (heap) */
    double      *prnu_col_gains[4];                     /* Per-column PRNU gain variation (heap) */
    uint16_t    *precomputed_prnu_map;                  /* Precomputed PRNU map: fixed-point gain × 16384 (heap) */
} fpn_model;

/*! @brief Generate Gaussian noise sample from PRNG state (shared by encoder/decoder) */
double noise_prng_gaussian(uint32_t *state);

/*! @brief Add noise to a pixel array from model parameters (shared by encoder/decoder) */
void noise_add_to_pixels(int32_t *data, int width, int height, int pitch_bytes,
                         double sigma, uint32_t seed, int band_id);

/*!
    @brief Remove noise by quantizing each pixel to its noise floor (encoder side)

    For each pixel: compute sigma from the noise model, then quantize the
    pixel value to the nearest multiple of sigma. The result is a clean
    signal with no noise — minimal entropy for the compressor.

    Error bound: ±sigma/2 per pixel (within noise floor, no signal loss).

    @param raw          Raw uint16 Bayer pixel data (modified in-place)
    @param width        Image width
    @param height       Image height
    @param noise_scale  DNG NoiseProfile scale (Poisson component)
    @param noise_offset DNG NoiseProfile offset (Gaussian component)
*/
void noise_remove(uint16_t *raw, int width, int height,
                  double noise_scale, double noise_offset);

/*!
    @brief Restore statistically equivalent noise after decompression (decoder side)

    For each pixel: compute sigma from the noise model, then add PRNG
    noise with matching variance. The PRNG is seeded deterministically
    so the decoder produces the same noise pattern as the original.

    Must be called with the same noise_scale, noise_offset, and seed
    that were used during encoding.

    @param raw          Raw uint16 Bayer pixel data (modified in-place)
    @param width        Image width
    @param height       Image height
    @param noise_scale  DNG NoiseProfile scale (Poisson component)
    @param noise_offset DNG NoiseProfile offset (Gaussian component)
    @param seed         PRNG seed (must match encoder)
*/
void noise_restore(uint16_t *raw, int width, int height,
                   double noise_scale, double noise_offset, uint32_t seed);

/*! @brief Legacy: remove noise then immediately add it back (no compression benefit) */
void noise_replace(uint16_t *raw, int width, int height,
                   double noise_scale, double noise_offset, uint32_t seed);

/*!
    @brief Auto-estimate Poisson-Gaussian noise model from raw pixels

    Fits variance = noise_scale * signal + noise_offset by binning pixels
    by signal level and measuring local noise in each bin. Uses adjacent-pixel
    differences (robust to image content).

    @param raw          Raw uint16 Bayer pixel data
    @param width        Image width
    @param height       Image height
    @param[out] noise_scale  Poisson component (slope)
    @param[out] noise_offset Gaussian component (intercept)
*/
void noise_estimate_model(const uint16_t *raw, int width, int height,
                          double *noise_scale, double *noise_offset);

/*! @brief Initialize FPN model to invalid/empty state */
void fpn_model_init(fpn_model *model);

/*! @brief Free heap-allocated FPN model data */
void fpn_model_free(fpn_model *model);

/*! @brief Load FPN model from JSON calibration file */
int fpn_model_load(fpn_model *model, const char *json_path);

/*! @brief Evaluate FPN at a specific pixel position
    @param model    Loaded FPN model
    @param row      Pixel row (0-based, full Bayer grid)
    @param col      Pixel column (0-based, full Bayer grid)
    @return         Estimated FPN offset for this pixel (add channel_mean for absolute dark level)
*/
double fpn_model_eval(const fpn_model *model, int row, int col);

/*! @brief Apply all noise corrections to a single Bayer pixel (for use inside UnpackPixel)
    @param model    Loaded noise model
    @param value    Raw pixel value
    @param row      Full Bayer grid row
    @param col      Full Bayer grid column
    @return         Corrected pixel value
*/
uint16_t noise_correct_pixel(const fpn_model *model, uint16_t value, int row, int col);

/*! @brief Restore all noise to a single pixel (inverse of noise_correct_pixel)
    @param model    Loaded noise model
    @param value    Corrected pixel value
    @param row      Full Bayer grid row
    @param col      Full Bayer grid column
    @return         Original (noise-restored) pixel value
*/
uint16_t noise_restore_pixel(const fpn_model *model, uint16_t value, int row, int col);

/*! @brief Subtract FPN from raw Bayer pixels in-place
    @param model    Loaded FPN model
    @param raw      Raw uint16 Bayer pixel data
    @param width    Image width
    @param height   Image height
*/
void fpn_subtract(const fpn_model *model, uint16_t *raw, int width, int height);

/*! @brief Add FPN back to raw Bayer pixels in-place (for noise reconstruction)
    @param model    Loaded FPN model
    @param raw      Raw uint16 Bayer pixel data
    @param width    Image width
    @param height   Image height
*/
void fpn_add_back(const fpn_model *model, uint16_t *raw, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* NOISE_MODEL_H */
