/*! @file noise_model.c
 *
 *  @brief Fixed-pattern noise model implementation.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include "noise_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared PRNG for noise reconstruction (used by both encoder and decoder) */

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

double noise_prng_gaussian(uint32_t *state)
{
    /* Box-Muller transform: 2 uniforms → 1 Gaussian.
       Much faster than CLT (2 xorshift + trig vs 12 xorshift). */
    double u1 = ((double)xorshift32(state) + 1.0) / 4294967297.0; /* (0,1) */
    double u2 = (double)xorshift32(state) / 4294967296.0;          /* [0,1) */
    return sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2);
}

void noise_add_to_pixels(int32_t *data, int width, int height, int pitch_bytes,
                         double sigma, uint32_t seed, int band_id)
{
    if (sigma <= 0.0) return;
    uint32_t state = seed ^ ((uint32_t)band_id * 2654435761u);
    if (state == 0) state = 1;
    int pitch_elems = pitch_bytes / (int)sizeof(int32_t);

    for (int row = 0; row < height; row++)
    {
        int32_t *row_ptr = data + row * pitch_elems;
        for (int col = 0; col < width; col++)
        {
            double noise = noise_prng_gaussian(&state) * sigma;
            row_ptr[col] += (int32_t)(noise + (noise >= 0 ? 0.5 : -0.5));
        }
    }
}

void noise_estimate_model(const uint16_t *raw, int width, int height,
                          double *noise_scale, double *noise_offset)
{
    *noise_scale = 0.0;
    *noise_offset = 0.0;

    if (width < 4 || height < 4) return;

    /* Bin pixels by signal level (8 bins), compute noise variance in each */
    #define NOISE_EST_BINS 8
    double bin_sum_signal[NOISE_EST_BINS] = {0};
    double bin_sum_var[NOISE_EST_BINS] = {0};
    int bin_count[NOISE_EST_BINS] = {0};

    /* Subsample: use every 4th row, every 4th column for speed */
    for (int row = 0; row < height; row += 4)
    {
        for (int col = 0; col < width - 2; col += 4)
        {
            int idx = row * width + col;
            double v0 = (double)raw[idx];
            double v2 = (double)raw[idx + 2]; /* Same Bayer channel, 2 apart */
            double signal = (v0 + v2) * 0.5;
            double diff = v2 - v0;
            double noise_var = diff * diff * 0.5; /* var = diff²/2 */

            int bin = (int)(signal * NOISE_EST_BINS / 16384.0);
            if (bin < 0) bin = 0;
            if (bin >= NOISE_EST_BINS) bin = NOISE_EST_BINS - 1;

            bin_sum_signal[bin] += signal;
            bin_sum_var[bin] += noise_var;
            bin_count[bin]++;
        }
    }

    /* Linear regression: variance = scale * signal + offset
       Using bins with enough samples */
    double sum_x = 0, sum_y = 0, sum_xx = 0, sum_xy = 0;
    int n = 0;
    for (int i = 0; i < NOISE_EST_BINS; i++)
    {
        if (bin_count[i] < 100) continue;
        double x = bin_sum_signal[i] / bin_count[i]; /* mean signal in bin */
        double y = bin_sum_var[i] / bin_count[i];     /* mean variance in bin */
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        n++;
    }

    if (n >= 2)
    {
        double denom = n * sum_xx - sum_x * sum_x;
        if (denom > 0)
        {
            *noise_scale = (n * sum_xy - sum_x * sum_y) / denom;
            *noise_offset = (sum_y - *noise_scale * sum_x) / n;
            if (*noise_scale < 0) *noise_scale = 0;
            if (*noise_offset < 0) *noise_offset = 0;
        }
    }
    else if (n == 1)
    {
        /* Single bin: assume all Gaussian */
        *noise_offset = sum_y / n;
    }
    #undef NOISE_EST_BINS
}

void noise_remove(uint16_t *raw, int width, int height,
                  double noise_scale, double noise_offset)
{
    if (noise_scale <= 0 && noise_offset <= 0) return;

    /* Precompute sigma lookup table to avoid per-pixel sqrt */
    #define NR_LUT_SHIFT 4
    #define NR_LUT_SIZE ((65536 >> NR_LUT_SHIFT) + 2)
    float *sigma_lut = (float *)malloc(NR_LUT_SIZE * sizeof(float));
    if (!sigma_lut) return;
    for (int k = 0; k < NR_LUT_SIZE; k++)
    {
        double sig = (double)(k << NR_LUT_SHIFT);
        double var = noise_scale * sig + noise_offset;
        if (var < 1.0) var = 1.0;
        sigma_lut[k] = (float)sqrt(var);
    }

    int total = width * height;
    for (int i = 0; i < total; i++)
    {
        float signal = (float)raw[i];
        int idx = raw[i] >> NR_LUT_SHIFT;
        float step = sigma_lut[idx];
        if (step < 1.0f) step = 1.0f;

        /* Quantize to noise-aware step: lose at most ±step/2 per pixel */
        float quantized = roundf(signal / step) * step;
        if (quantized < 0) quantized = 0;
        if (quantized > 65535) quantized = 65535;
        raw[i] = (uint16_t)(quantized + 0.5f);
    }

    free(sigma_lut);
    #undef NR_LUT_SHIFT
    #undef NR_LUT_SIZE
}

void noise_restore(uint16_t *raw, int width, int height,
                   double noise_scale, double noise_offset, uint32_t seed)
{
    if (noise_scale <= 0 && noise_offset <= 0) return;

    uint32_t state = seed;
    if (state == 0) state = 0x12345678;

    /* Precompute sqrt lookup table for common signal values.
       For 16-bit signals (0-65535), variance = scale * signal + offset.
       We precompute sqrt(variance) * 0.5 for every 16th signal level
       and interpolate. This avoids per-pixel sqrt calls. */
    #define SQRT_LUT_SHIFT 4
    #define SQRT_LUT_SIZE ((65536 >> SQRT_LUT_SHIFT) + 2)
    float *sigma_lut = (float *)malloc(SQRT_LUT_SIZE * sizeof(float));
    if (!sigma_lut) return;
    for (int k = 0; k < SQRT_LUT_SIZE; k++)
    {
        double sig = (double)(k << SQRT_LUT_SHIFT);
        double var = noise_scale * sig + noise_offset;
        if (var < 1.0) var = 1.0;
        sigma_lut[k] = (float)(sqrt(var) * 0.5);
    }

    int total = width * height;

    /* Triangular noise: sum of 2 uniforms − 1 gives zero-mean triangular
       distribution with variance = 1/6. Scale by sqrt(6) ≈ 2.449 for
       unit variance. Only 2 xorshift per pixel (50% faster than Irwin-Hall).
       Perceptually indistinguishable from Gaussian for noise reconstruction. */
    const float scale_factor = 2.4494897f; /* sqrt(6) */
    const float inv_u32 = 1.0f / 4294967296.0f;

    for (int i = 0; i < total; i++)
    {
        float u = (float)xorshift32(&state) * inv_u32
                + (float)xorshift32(&state) * inv_u32
                - 1.0f;
        float g = u * scale_factor;

        /* Look up sigma from precomputed table */
        int sig_val = raw[i];
        int idx = sig_val >> SQRT_LUT_SHIFT;
        float half_sigma = sigma_lut[idx];

        float result = (float)sig_val + g * half_sigma;
        if (result < 0) result = 0;
        if (result > 65535) result = 65535;
        raw[i] = (uint16_t)(result + 0.5f);
    }

    free(sigma_lut);
    #undef SQRT_LUT_SHIFT
    #undef SQRT_LUT_SIZE
}

/* Legacy combined function (deprecated — use noise_remove + noise_restore) */
void noise_replace(uint16_t *raw, int width, int height,
                   double noise_scale, double noise_offset, uint32_t seed)
{
    noise_remove(raw, width, height, noise_scale, noise_offset);
    noise_restore(raw, width, height, noise_scale, noise_offset, seed);
}

void fpn_model_init(fpn_model *model)
{
    memset(model, 0, sizeof(fpn_model));
    for (int ch = 0; ch < 4; ch++)
    {
        model->row_offsets[ch] = NULL;
        model->col_offsets[ch] = NULL;
        model->prnu_row_gains[ch] = NULL;
        model->prnu_col_gains[ch] = NULL;
    }
    model->precomputed_map = NULL;
    model->precomputed_prnu_map = NULL;
}

void fpn_model_free(fpn_model *model)
{
    for (int ch = 0; ch < 4; ch++)
    {
        free(model->row_offsets[ch]);
        free(model->col_offsets[ch]);
        free(model->prnu_row_gains[ch]);
        free(model->prnu_col_gains[ch]);
        model->row_offsets[ch] = NULL;
        model->col_offsets[ch] = NULL;
        model->prnu_row_gains[ch] = NULL;
        model->prnu_col_gains[ch] = NULL;
    }
    free(model->precomputed_map);
    free(model->precomputed_prnu_map);
    model->precomputed_map = NULL;
    model->precomputed_prnu_map = NULL;
}

/*! Precompute the full-resolution FPN map for fast subtraction/addition.
    Evaluates polynomial + row/col offsets once per pixel at load time. */
static void fpn_precompute(fpn_model *model)
{
    if (!model->valid || model->width <= 0 || model->height <= 0) return;

    size_t npixels = (size_t)model->width * model->height;
    model->precomputed_map = (int16_t *)calloc(npixels, sizeof(int16_t));
    if (!model->precomputed_map) return;

    for (int row = 0; row < model->height; row++)
    {
        for (int col = 0; col < model->width; col++)
        {
            double fpn = fpn_model_eval(model, row, col);
            int32_t rounded = (int32_t)(fpn + (fpn >= 0 ? 0.5 : -0.5));
            if (rounded > 32767) rounded = 32767;
            if (rounded < -32768) rounded = -32768;
            model->precomputed_map[row * model->width + col] = (int16_t)rounded;
        }
    }
}

/* Simple JSON number array parser (no external dependency) */
static int parse_double_array(const char *json, const char *key, double *out, int max_count)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return 0;

    p = strchr(p, '[');
    if (!p) return 0;
    p++;

    int count = 0;
    while (count < max_count)
    {
        char *end;
        double val = strtod(p, &end);
        if (end == p) break;
        out[count++] = val;
        p = end;
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
    }
    return count;
}

static int parse_uint32(const char *json, const char *key, uint32_t *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    *out = (uint32_t)strtoul(p, NULL, 10);
    return 1;
}

static int parse_int(const char *json, const char *key, int *out)
{
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

int fpn_model_load(fpn_model *model, const char *json_path)
{
    fpn_model_init(model);

    FILE *f = fopen(json_path, "r");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *json = (char *)malloc(size + 1);
    if (!json) { fclose(f); return -1; }
    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    parse_int(json, "width", &model->width);
    parse_int(json, "height", &model->height);
    parse_int(json, "fpn_poly_order", &model->poly_order);
    if (model->poly_order > FPN_MAX_POLY_ORDER)
        model->poly_order = FPN_MAX_POLY_ORDER;
    parse_uint32(json, "fpn_seed", &model->seed);

    parse_double_array(json, "channel_means", model->channel_means, 4);
    parse_double_array(json, "residual_sigma", model->residual_sigma, 4);
    parse_double_array(json, "fpn_poly_R", model->poly_coeffs[0], FPN_MAX_POLY_TERMS);
    parse_double_array(json, "fpn_poly_Gr", model->poly_coeffs[1], FPN_MAX_POLY_TERMS);
    parse_double_array(json, "fpn_poly_Gb", model->poly_coeffs[2], FPN_MAX_POLY_TERMS);
    parse_double_array(json, "fpn_poly_B", model->poly_coeffs[3], FPN_MAX_POLY_TERMS);

    /* Load row/column offsets if present (before freeing json!) */
    int half_h = model->height / 2;
    int half_w = model->width / 2;
    const char *row_keys[] = {"row_offsets_R", "row_offsets_Gr", "row_offsets_Gb", "row_offsets_B"};
    const char *col_keys[] = {"col_offsets_R", "col_offsets_Gr", "col_offsets_Gb", "col_offsets_B"};
    int has_rc = 0;

    for (int ch = 0; ch < 4; ch++)
    {
        if (strstr(json, row_keys[ch]) != NULL && half_h > 0)
        {
            model->row_offsets[ch] = (double *)calloc(half_h, sizeof(double));
            if (model->row_offsets[ch])
            {
                int n = parse_double_array(json, row_keys[ch], model->row_offsets[ch], half_h);
                if (n > 0) has_rc = 1;
            }
        }
        if (strstr(json, col_keys[ch]) != NULL && half_w > 0)
        {
            model->col_offsets[ch] = (double *)calloc(half_w, sizeof(double));
            if (model->col_offsets[ch])
            {
                int n = parse_double_array(json, col_keys[ch], model->col_offsets[ch], half_w);
                if (n > 0) has_rc = 1;
            }
        }
    }
    model->has_row_col_offsets = has_rc;
    model->half_rows = half_h;
    model->half_cols = half_w;

    free(json);

    if (model->width > 0 && model->height > 0 && model->poly_order > 0)
        model->valid = 1;

    /* Precompute the full-resolution FPN map for fast per-image subtraction */
    if (model->valid)
        fpn_precompute(model);

    return model->valid ? 0 : -1;
}

/* Evaluate 2D polynomial at normalized coordinates using precomputed powers */
static double eval_poly(const double *coeffs, int order, double x, double y)
{
    /* Precompute powers: avoids 60 pow() calls per pixel */
    double xp[FPN_MAX_POLY_ORDER + 1], yp[FPN_MAX_POLY_ORDER + 1];
    xp[0] = 1.0; yp[0] = 1.0;
    for (int k = 1; k <= order; k++)
    {
        xp[k] = xp[k-1] * x;
        yp[k] = yp[k-1] * y;
    }

    double result = 0;
    int idx = 0;
    for (int total = 0; total <= order; total++)
        for (int i = total; i >= 0; i--)
            result += coeffs[idx++] * xp[i] * yp[total - i];
    return result;
}

double fpn_model_eval(const fpn_model *model, int row, int col)
{
    if (!model->valid) return 0.0;

    /* Determine Bayer channel: 0=R(even row, even col), 1=Gr, 2=Gb, 3=B */
    int ch = (row & 1) * 2 + (col & 1);
    int hr = row / 2, hc = col / 2;

    /* Normalize coordinates to [-1, 1] using half-resolution grid */
    int half_w = model->width / 2;
    int half_h = model->height / 2;
    double x = (half_w > 1) ? (2.0 * hc / (half_w - 1)) - 1.0 : 0.0;
    double y = (half_h > 1) ? (2.0 * hr / (half_h - 1)) - 1.0 : 0.0;

    double fpn = eval_poly(model->poly_coeffs[ch], model->poly_order, x, y);

    /* Add row/column banding offsets if available */
    if (model->has_row_col_offsets)
    {
        if (model->row_offsets[ch] && hr < model->half_rows)
            fpn += model->row_offsets[ch][hr];
        if (model->col_offsets[ch] && hc < model->half_cols)
            fpn += model->col_offsets[ch][hc];
    }

    return fpn;
}

uint16_t noise_correct_pixel(const fpn_model *model, uint16_t value, int row, int col)
{
    if (!model->valid) return value;

    int idx = row * model->width + col;
    int32_t corrected = (int32_t)value;

    /* Step 1: DSNU subtraction (additive dark frame) */
    if (model->precomputed_map && row < model->height && col < model->width)
        corrected -= (int32_t)model->precomputed_map[idx];

    /* Step 2: PRNU correction (multiplicative gain normalization) */
    if (model->has_prnu && model->precomputed_prnu_map && row < model->height && col < model->width)
    {
        /* precomputed_prnu_map stores gain × 16384 (fixed-point 2.14) */
        uint16_t gain_fp = model->precomputed_prnu_map[idx];
        if (gain_fp > 0)
            corrected = (int32_t)((int64_t)corrected * 16384 / gain_fp);
    }

    if (corrected < 0) corrected = 0;
    if (corrected > 65535) corrected = 65535;
    return (uint16_t)corrected;
}

uint16_t noise_restore_pixel(const fpn_model *model, uint16_t value, int row, int col)
{
    if (!model->valid) return value;

    int idx = row * model->width + col;
    int32_t restored = (int32_t)value;

    /* Reverse order: PRNU first (multiply), then DSNU (add) */

    /* Step 9: PRNU restore (multiply by gain) */
    if (model->has_prnu && model->precomputed_prnu_map && row < model->height && col < model->width)
    {
        uint16_t gain_fp = model->precomputed_prnu_map[idx];
        restored = (int32_t)((int64_t)restored * gain_fp / 16384);
    }

    /* Step 10: DSNU restore (add dark frame) */
    if (model->precomputed_map && row < model->height && col < model->width)
        restored += (int32_t)model->precomputed_map[idx];

    if (restored < 0) restored = 0;
    if (restored > 65535) restored = 65535;
    return (uint16_t)restored;
}

void fpn_subtract(const fpn_model *model, uint16_t *raw, int width, int height)
{
    if (!model->valid) return;

    /* Apply full noise correction pipeline: DSNU subtract → PRNU divide
       Uses precomputed maps for O(1) per pixel via noise_correct_pixel() */
    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++)
            raw[row * width + col] = noise_correct_pixel(model, raw[row * width + col], row, col);
}

void fpn_add_back(const fpn_model *model, uint16_t *raw, int width, int height)
{
    if (!model->valid) return;

    /* Reverse noise correction: PRNU multiply → DSNU add
       Uses precomputed maps for O(1) per pixel via noise_restore_pixel() */
    for (int row = 0; row < height; row++)
        for (int col = 0; col < width; col++)
            raw[row * width + col] = noise_restore_pixel(model, raw[row * width + col], row, col);
}
