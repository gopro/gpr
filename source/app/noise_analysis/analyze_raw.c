/*! @file analyze_raw.c
 *
 *  @brief Standalone raw pixel noise analyzer.
 *
 *  Reads a raw Bayer pixel file and estimates sensor noise characteristics:
 *  - Read noise (from horizontal pixel pair differences)
 *  - Poisson-Gaussian noise model fit (variance vs signal)
 *  - Dark region statistics
 *  - Spatial noise pattern analysis (FPN indicators)
 *
 *  Usage: analyze_raw <input.RAW> <width> <height> <bits>
 *         (decode GPR to RAW first with: gpr_tools -i input.GPR -o input.RAW)
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int compare_int32(const void *a, const void *b)
{
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

/* Estimate noise sigma from horizontal same-color pixel differences */
static double estimate_read_noise(const uint16_t *raw, int w, int h)
{
    int max_pairs = w * h / 4;
    int32_t *diffs = (int32_t *)malloc(max_pairs * sizeof(int32_t));
    if (!diffs) return 0.0;
    int count = 0;

    for (int row = 0; row < h && count < max_pairs; row++)
    {
        const uint16_t *r = raw + row * w;
        for (int col = 2; col < w - 2 && count < max_pairs; col += 4)
        {
            int32_t d = (int32_t)r[col] - (int32_t)r[col + 2];
            diffs[count++] = (d < 0) ? -d : d;
        }
    }

    qsort(diffs, count, sizeof(int32_t), compare_int32);
    double median = (count > 0) ? (double)diffs[count / 2] : 0.0;
    free(diffs);
    return median / 0.6745 / 1.41421356;
}

/* Fit Poisson-Gaussian model: variance = scale * signal + offset */
static void fit_noise_model(const uint16_t *raw, int w, int h, int bits,
                            double *scale, double *offset)
{
    int max_val = (1 << bits) - 1;
    int num_bins = 64;
    int bin_size = (max_val + 1) / num_bins;
    if (bin_size < 1) bin_size = 1;

    double *bin_mean = (double *)calloc(num_bins, sizeof(double));
    double *bin_var = (double *)calloc(num_bins, sizeof(double));
    int *bin_n = (int *)calloc(num_bins, sizeof(int));

    for (int row = 0; row < h; row++)
    {
        const uint16_t *r = raw + row * w;
        for (int col = 2; col < w - 2; col += 4)
        {
            int32_t p1 = r[col], p2 = r[col + 2];
            int32_t mean = (p1 + p2) / 2;
            int32_t diff = p1 - p2;
            int bin = mean / bin_size;
            if (bin >= num_bins) bin = num_bins - 1;
            bin_mean[bin] += mean;
            bin_var[bin] += (double)diff * diff;
            bin_n[bin]++;
        }
    }

    /* Least squares fit: var = scale * mean + offset */
    double sx = 0, sy = 0, sxy = 0, sxx = 0;
    int n = 0;
    printf("\n  Noise vs Signal (binned):\n");
    printf("  %10s %10s %8s\n", "Signal", "Variance", "Count");
    for (int b = 0; b < num_bins; b++)
    {
        if (bin_n[b] < 50) continue;
        double x = bin_mean[b] / bin_n[b];
        double y = bin_var[b] / bin_n[b] / 2.0;
        if (n < 10 || b % (num_bins/10) == 0)
            printf("  %10.0f %10.1f %8d\n", x, y, bin_n[b]);
        sx += x; sy += y; sxy += x * y; sxx += x * x;
        n++;
    }

    if (n >= 2)
    {
        double denom = n * sxx - sx * sx;
        if (denom > 0)
        {
            *scale = (n * sxy - sx * sy) / denom;
            *offset = (sy - *scale * sx) / n;
            if (*scale < 0) *scale = 0;
            if (*offset < 0) *offset = 0;
        }
    }

    free(bin_mean); free(bin_var); free(bin_n);
}

/* Analyze spatial noise patterns for FPN detection */
static void analyze_spatial_patterns(const uint16_t *raw, int w, int h, int bits)
{
    /* Compute per-row and per-column mean to detect banding (FPN) */
    double *row_means = (double *)calloc(h, sizeof(double));
    double *col_means = (double *)calloc(w, sizeof(double));

    for (int row = 0; row < h; row++)
    {
        double sum = 0;
        const uint16_t *r = raw + row * w;
        for (int col = 0; col < w; col++) sum += r[col];
        row_means[row] = sum / w;
    }
    for (int col = 0; col < w; col++)
    {
        double sum = 0;
        for (int row = 0; row < h; row++) sum += raw[row * w + col];
        col_means[col] = sum / h;
    }

    /* Compute row-to-row and col-to-col variation (FPN indicator) */
    double row_var = 0, col_var = 0;
    double row_grand_mean = 0, col_grand_mean = 0;
    for (int r = 0; r < h; r++) row_grand_mean += row_means[r];
    row_grand_mean /= h;
    for (int c = 0; c < w; c++) col_grand_mean += col_means[c];
    col_grand_mean /= w;

    for (int r = 0; r < h; r++)
        row_var += (row_means[r] - row_grand_mean) * (row_means[r] - row_grand_mean);
    row_var /= h;

    for (int c = 0; c < w; c++)
        col_var += (col_means[c] - col_grand_mean) * (col_means[c] - col_grand_mean);
    col_var /= w;

    printf("\n  Spatial Pattern Analysis (FPN indicators):\n");
    printf("    Row mean variation (sigma): %.2f DN\n", sqrt(row_var));
    printf("    Col mean variation (sigma): %.2f DN\n", sqrt(col_var));
    printf("    Grand mean: %.1f DN (of %d max)\n", row_grand_mean, (1 << bits) - 1);

    if (sqrt(row_var) > 2.0 || sqrt(col_var) > 2.0)
        printf("    >> FPN detected: banding visible in %s direction\n",
               sqrt(row_var) > sqrt(col_var) ? "row (horizontal)" : "column (vertical)");
    else
        printf("    >> Low FPN: no significant banding detected\n");

    free(row_means); free(col_means);
}

/* Dark region analysis */
static void analyze_dark_regions(const uint16_t *raw, int w, int h)
{
    /* Find darkest 5% of pixels */
    int total = w * h;
    int target = total / 20;
    uint32_t hist[4096] = {0};

    for (int i = 0; i < total; i++)
    {
        int bin = raw[i] >> 4;
        if (bin > 4095) bin = 4095;
        hist[bin]++;
    }

    int threshold = 0, cum = 0;
    for (int b = 0; b < 4096; b++)
    {
        cum += hist[b];
        if (cum >= target) { threshold = (b + 1) << 4; break; }
    }

    double sum = 0, sumsq = 0;
    int count = 0;
    for (int i = 0; i < total; i++)
    {
        if (raw[i] <= threshold)
        {
            sum += raw[i]; sumsq += (double)raw[i] * raw[i]; count++;
        }
    }

    if (count > 0)
    {
        double mean = sum / count;
        double var = sumsq / count - mean * mean;
        printf("\n  Dark Region Analysis (darkest 5%%):\n");
        printf("    Threshold: %d DN\n", threshold);
        printf("    Dark pixel count: %d (%.1f%%)\n", count, 100.0 * count / total);
        printf("    Dark mean: %.1f DN\n", mean);
        printf("    Dark sigma: %.2f DN (read noise floor)\n", sqrt(var > 0 ? var : 0));
    }
}

/* Compute entropy of raw data */
static double compute_entropy(const uint16_t *raw, int w, int h, int bits)
{
    int max_val = 1 << bits;
    int *hist = (int *)calloc(max_val, sizeof(int));
    if (!hist) return 0.0;

    int total = w * h;
    for (int i = 0; i < total; i++)
    {
        int v = raw[i];
        if (v >= max_val) v = max_val - 1;
        hist[v]++;
    }

    double entropy = 0.0;
    for (int i = 0; i < max_val; i++)
    {
        if (hist[i] > 0)
        {
            double p = (double)hist[i] / total;
            entropy -= p * log2(p);
        }
    }

    free(hist);
    return entropy;
}

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        fprintf(stderr, "Usage: %s <input.RAW> <width> <height> <bits>\n", argv[0]);
        fprintf(stderr, "\nAnalyze raw Bayer sensor noise characteristics.\n");
        fprintf(stderr, "Decode GPR first: gpr_tools -i input.GPR -o input.RAW\n");
        return 1;
    }

    const char *path = argv[1];
    int w = atoi(argv[2]);
    int h = atoi(argv[3]);
    int bits = atoi(argv[4]);

    size_t expected_size = (size_t)w * h * sizeof(uint16_t);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return 1; }

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size != expected_size)
    {
        fprintf(stderr, "Size mismatch: expected %zu bytes (%dx%d x 2), got %zu\n",
                expected_size, w, h, file_size);
        fclose(f);
        return 1;
    }

    uint16_t *raw = (uint16_t *)malloc(file_size);
    fread(raw, 1, file_size, f);
    fclose(f);

    printf("=== Sensor Noise Analysis ===\n");
    printf("  File: %s\n", path);
    printf("  Dimensions: %d x %d, %d-bit\n", w, h, bits);
    printf("  Pixels: %d\n", w * h);

    /* Read noise estimation */
    double read_noise = estimate_read_noise(raw, w, h);
    printf("\n  Read Noise (MAD estimator): %.2f DN\n", read_noise);

    /* Poisson-Gaussian model fit */
    double scale = 0, offset = 0;
    fit_noise_model(raw, w, h, bits, &scale, &offset);
    printf("\n  Poisson-Gaussian Model Fit:\n");
    printf("    noise_scale  = %.8f (photon gain)\n", scale);
    printf("    noise_offset = %.4f (read noise variance)\n", offset);
    printf("    Predicted read noise: %.2f DN (sqrt(offset))\n", sqrt(offset > 0 ? offset : 0));

    /* Dark region analysis */
    analyze_dark_regions(raw, w, h);

    /* Spatial pattern analysis */
    analyze_spatial_patterns(raw, w, h, bits);

    /* Entropy analysis */
    double entropy = compute_entropy(raw, w, h, bits);
    double theoretical_min = entropy * w * h / 8.0;
    printf("\n  Entropy Analysis:\n");
    printf("    Shannon entropy: %.2f bits/pixel\n", entropy);
    printf("    Theoretical minimum size: %.1f MB\n", theoretical_min / 1048576.0);
    printf("    Raw file size: %.1f MB\n", (double)file_size / 1048576.0);
    printf("    Max compression ratio: %.1fx\n", (double)(w * h * bits) / (entropy * w * h));

    printf("\n  Recommended GPR denoise parameters:\n");
    printf("    --Denoise -N %d  (based on noise/signal ratio)\n",
           (int)(read_noise * 10.0 / ((1 << bits) / 256.0)));

    free(raw);
    return 0;
}
