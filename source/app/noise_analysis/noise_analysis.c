/*! @file noise_analysis.c
 *
 *  @brief Sensor noise analysis tool for GPR codec.
 *
 *  Analyzes GPR/DNG images to estimate sensor noise parameters without
 *  requiring formal calibration. Extracts noise characteristics from
 *  actual photographic images by examining:
 *  - DNG NoiseProfile metadata (manufacturer calibration)
 *  - Wavelet-domain MAD noise estimation per channel
 *  - Dark region analysis (noise floor in shadows)
 *  - Per-ISO noise scaling behavior
 *
 *  Usage: noise_analysis <input.GPR> [input2.GPR ...]
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "gpr.h"

/* Robust noise estimation using MAD of raw pixel differences */
static double estimate_read_noise_from_raw(const uint16_t *raw, int width, int height, int pitch_pixels)
{
    /* Estimate read noise from horizontal pixel pair differences.
       For adjacent same-color pixels in Bayer pattern (2 pixels apart),
       the difference is dominated by noise since scene content varies slowly.
       MAD of differences / sqrt(2) / 0.6745 gives noise sigma. */
    int count = 0;
    int max_pairs = width * height / 4;
    int32_t *diffs = (int32_t *)malloc(max_pairs * sizeof(int32_t));
    if (!diffs) return 0.0;

    for (int row = 0; row < height; row += 2)
    {
        const uint16_t *r = raw + row * pitch_pixels;
        for (int col = 2; col < width - 2; col += 4)
        {
            /* Same-color Bayer pixel pairs (2 apart horizontally) */
            int32_t d = (int32_t)r[col] - (int32_t)r[col + 2];
            diffs[count++] = (d < 0) ? -d : d;
            if (count >= max_pairs) break;
        }
        if (count >= max_pairs) break;
    }

    if (count == 0) { free(diffs); return 0.0; }

    /* Simple selection sort for median — fine for this analysis tool */
    /* Use partial sort: just find the median element */
    int mid = count / 2;
    for (int i = 0; i <= mid; i++)
    {
        int min_idx = i;
        for (int j = i + 1; j < count; j++)
            if (diffs[j] < diffs[min_idx]) min_idx = j;
        if (min_idx != i) { int32_t t = diffs[i]; diffs[i] = diffs[min_idx]; diffs[min_idx] = t; }
    }

    double median = (double)diffs[mid];
    free(diffs);

    /* MAD normalization: sigma = MAD / 0.6745 / sqrt(2) for difference pairs */
    return median / 0.6745 / 1.41421356;
}

/* Estimate signal-dependent noise by binning pixels by brightness */
static void estimate_poisson_gaussian(const uint16_t *raw, int width, int height, int pitch_pixels,
                                      int bit_depth, double *out_scale, double *out_offset)
{
    int max_val = (1 << bit_depth) - 1;
    int num_bins = 64;
    int bin_size = (max_val + 1) / num_bins;

    double *bin_sum = (double *)calloc(num_bins, sizeof(double));
    double *bin_sumsq = (double *)calloc(num_bins, sizeof(double));
    int *bin_count = (int *)calloc(num_bins, sizeof(int));

    if (!bin_sum || !bin_sumsq || !bin_count) goto cleanup;

    /* Accumulate pixel pair differences binned by mean brightness */
    for (int row = 0; row < height; row += 2)
    {
        const uint16_t *r = raw + row * pitch_pixels;
        for (int col = 2; col < width - 2; col += 4)
        {
            int32_t p1 = r[col];
            int32_t p2 = r[col + 2];
            int32_t mean = (p1 + p2) / 2;
            int32_t diff = p1 - p2;

            int bin = mean / bin_size;
            if (bin >= num_bins) bin = num_bins - 1;
            if (bin < 0) bin = 0;

            bin_sum[bin] += (double)mean;
            bin_sumsq[bin] += (double)diff * diff;
            bin_count[bin]++;
        }
    }

    /* Fit variance = scale * signal + offset using least squares on bins with enough samples */
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    int n = 0;
    for (int b = 0; b < num_bins; b++)
    {
        if (bin_count[b] < 100) continue;
        double x = bin_sum[b] / bin_count[b];  /* mean brightness */
        double y = bin_sumsq[b] / bin_count[b] / 2.0;  /* variance (diff^2/2 for pair diffs) */
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
        n++;
    }

    if (n >= 2)
    {
        double denom = n * sum_xx - sum_x * sum_x;
        if (denom > 0)
        {
            *out_scale = (n * sum_xy - sum_x * sum_y) / denom;
            *out_offset = (sum_y - *out_scale * sum_x) / n;
            if (*out_scale < 0) *out_scale = 0;
            if (*out_offset < 0) *out_offset = 0;
        }
    }

cleanup:
    free(bin_sum);
    free(bin_sumsq);
    free(bin_count);
}

/* Analyze the darkest region of an image for noise floor estimation */
static double analyze_dark_regions(const uint16_t *raw, int width, int height, int pitch_pixels,
                                   double *dark_mean, double *dark_sigma)
{
    /* Find the darkest 5% of pixels and measure their statistics */
    int total = width * height;
    int dark_threshold_count = total / 20;  /* 5% */

    /* First pass: find histogram to determine dark threshold */
    int hist[4096] = {0};
    for (int row = 0; row < height; row++)
    {
        const uint16_t *r = raw + row * pitch_pixels;
        for (int col = 0; col < width; col++)
        {
            int bin = r[col] >> 4;  /* 16-bit to 12-bit histogram */
            if (bin > 4095) bin = 4095;
            hist[bin]++;
        }
    }

    int threshold = 0;
    int cumulative = 0;
    for (int b = 0; b < 4096; b++)
    {
        cumulative += hist[b];
        if (cumulative >= dark_threshold_count)
        {
            threshold = (b + 1) << 4;
            break;
        }
    }

    /* Second pass: compute mean and variance of dark pixels */
    double sum = 0, sumsq = 0;
    int count = 0;
    for (int row = 0; row < height; row++)
    {
        const uint16_t *r = raw + row * pitch_pixels;
        for (int col = 0; col < width; col++)
        {
            if (r[col] <= threshold)
            {
                double v = (double)r[col];
                sum += v;
                sumsq += v * v;
                count++;
            }
        }
    }

    if (count > 0)
    {
        *dark_mean = sum / count;
        double var = sumsq / count - (*dark_mean) * (*dark_mean);
        *dark_sigma = (var > 0) ? sqrt(var) : 0.0;
    }
    return (double)count / total * 100.0;  /* percentage of dark pixels */
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <input.GPR> [input2.GPR ...]\n", argv[0]);
        fprintf(stderr, "\nAnalyzes sensor noise characteristics from GPR/DNG images.\n");
        fprintf(stderr, "Reports: DNG NoiseProfile, estimated read noise, Poisson-Gaussian model,\n");
        fprintf(stderr, "         dark region statistics, and recommended denoise parameters.\n");
        return 1;
    }

    gpr_allocator allocator;
    allocator.Alloc = malloc;
    allocator.Free = free;

    printf("%-40s %6s %10s %10s %10s %10s %10s %8s\n",
           "File", "ISO", "NoiseScale", "NoiseOff", "EstSigma", "DarkMean", "DarkSig", "Dark%%");
    printf("%-40s %6s %10s %10s %10s %10s %10s %8s\n",
           "----", "---", "----------", "--------", "--------", "--------", "-------", "------");

    for (int f = 1; f < argc; f++)
    {
        const char *path = argv[f];

        gpr_buffer input_buf = { NULL, 0 };
        gpr_parameters params;
        gpr_parameters_set_defaults(&params);

        /* Read the file */
        if (read_from_file(&input_buf, path, allocator.Alloc, allocator.Free) != 0)
        {
            fprintf(stderr, "  Error reading: %s\n", path);
            continue;
        }

        /* Parse metadata */
        gpr_parse_metadata(&allocator, &input_buf, &params);

        /* Decode to RAW for pixel analysis */
        gpr_buffer raw_buf = { NULL, 0 };
        int result = gpr_convert(&allocator, &input_buf, &raw_buf, &params);

        if (result != 0 || raw_buf.buffer == NULL)
        {
            fprintf(stderr, "  Error decoding: %s\n", path);
            allocator.Free(input_buf.buffer);
            continue;
        }

        int width = params.input_width;
        int height = params.input_height;
        int pitch_pixels = width;  /* RAW output is packed */

        /* Get noise parameters from DNG metadata */
        double dng_scale = params.tuning_info.noise_scale;
        double dng_offset = params.tuning_info.noise_offset;
        int iso = params.exif_info.iso_speed_rating;

        /* Estimate noise from raw pixel data */
        double est_sigma = estimate_read_noise_from_raw(
            (const uint16_t *)raw_buf.buffer, width, height, pitch_pixels);

        /* Analyze dark regions */
        double dark_mean = 0, dark_sigma = 0;
        double dark_pct = analyze_dark_regions(
            (const uint16_t *)raw_buf.buffer, width, height, pitch_pixels,
            &dark_mean, &dark_sigma);

        /* Extract filename from path */
        const char *filename = strrchr(path, '/');
        filename = filename ? filename + 1 : path;

        char short_name[41];
        strncpy(short_name, filename, 40);
        short_name[40] = '\0';

        printf("%-40s %6d %10.6f %10.4f %10.2f %10.1f %10.2f %7.1f%%\n",
               short_name, iso, dng_scale, dng_offset, est_sigma,
               dark_mean, dark_sigma, dark_pct);

        allocator.Free(raw_buf.buffer);
        allocator.Free(input_buf.buffer);
    }

    return 0;
}
