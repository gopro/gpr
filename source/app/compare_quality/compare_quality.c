/*! @file compare_quality.c
 *
 *  @brief Comprehensive image quality comparison tool.
 *
 *  Compares two raw Bayer images with multiple quality metrics:
 *  - PSNR (peak signal-to-noise ratio)
 *  - SSIM (structural similarity index)
 *  - Per-region analysis (dark/mid/bright)
 *  - Noise statistics preservation (sigma comparison)
 *  - Histogram divergence
 *  - Maximum error and error distribution
 *
 *  Usage: compare_quality <original.RAW> <decoded.RAW> <width> <height> [bits]
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* SSIM constants (for 16-bit data) */
#define SSIM_K1 0.01
#define SSIM_K2 0.03
#define SSIM_WINDOW 8

static double compute_psnr(const uint16_t *a, const uint16_t *b, size_t n, int bits) {
    double max_val = (1 << bits) - 1;
    double sse = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)a[i] - (double)b[i];
        sse += d * d;
    }
    double mse = sse / n;
    if (mse == 0) return 999.0;
    return 10.0 * log10(max_val * max_val / mse);
}

/* Compute SSIM over a window */
static double ssim_window(const uint16_t *a, const uint16_t *b,
                          int w, int h, int x0, int y0, int win, int bits) {
    double L = (1 << bits) - 1;
    double c1 = (SSIM_K1 * L) * (SSIM_K1 * L);
    double c2 = (SSIM_K2 * L) * (SSIM_K2 * L);

    double sum_a = 0, sum_b = 0, sum_a2 = 0, sum_b2 = 0, sum_ab = 0;
    int count = 0;

    for (int dy = 0; dy < win && y0+dy < h; dy++) {
        for (int dx = 0; dx < win && x0+dx < w; dx++) {
            int idx = (y0+dy) * w + (x0+dx);
            double va = a[idx], vb = b[idx];
            sum_a += va; sum_b += vb;
            sum_a2 += va*va; sum_b2 += vb*vb;
            sum_ab += va*vb;
            count++;
        }
    }
    if (count == 0) return 1.0;

    double mu_a = sum_a / count, mu_b = sum_b / count;
    double sig_a2 = sum_a2 / count - mu_a * mu_a;
    double sig_b2 = sum_b2 / count - mu_b * mu_b;
    double sig_ab = sum_ab / count - mu_a * mu_b;

    double num = (2*mu_a*mu_b + c1) * (2*sig_ab + c2);
    double den = (mu_a*mu_a + mu_b*mu_b + c1) * (sig_a2 + sig_b2 + c2);
    return num / den;
}

static double compute_ssim(const uint16_t *a, const uint16_t *b,
                           int w, int h, int bits) {
    double sum = 0;
    int count = 0;
    for (int y = 0; y < h; y += SSIM_WINDOW) {
        for (int x = 0; x < w; x += SSIM_WINDOW) {
            sum += ssim_window(a, b, w, h, x, y, SSIM_WINDOW, bits);
            count++;
        }
    }
    return sum / count;
}

/* Estimate noise sigma from horizontal same-color pixel differences */
static double estimate_noise(const uint16_t *data, int w, int h) {
    int hist[32768] = {0};
    for (int r = 0; r < h; r += 2)
        for (int c = 2; c < w-2; c += 4) {
            int d = abs((int)data[r*w+c] - (int)data[r*w+c+2]);
            if (d < 32768) hist[d]++;
        }
    int total = 0;
    for (int i = 0; i < 32768; i++) total += hist[i];
    int mid = total / 2, cum = 0;
    for (int i = 0; i < 32768; i++) {
        cum += hist[i];
        if (cum >= mid) return (double)i / 0.6745 / 1.414;
    }
    return 0;
}

/* KL divergence between two histograms */
static double histogram_divergence(const uint16_t *a, const uint16_t *b,
                                   size_t n, int bits) {
    int nbins = 256;
    int shift = bits - 8;
    double *ha = (double *)calloc(nbins, sizeof(double));
    double *hb = (double *)calloc(nbins, sizeof(double));

    for (size_t i = 0; i < n; i++) {
        ha[a[i] >> shift]++;
        hb[b[i] >> shift]++;
    }
    /* Normalize */
    for (int i = 0; i < nbins; i++) { ha[i] /= n; hb[i] /= n; }

    /* Symmetric KL divergence */
    double kl = 0;
    for (int i = 0; i < nbins; i++) {
        if (ha[i] > 1e-10 && hb[i] > 1e-10) {
            kl += ha[i] * log2(ha[i] / hb[i]);
            kl += hb[i] * log2(hb[i] / ha[i]);
        }
    }
    free(ha); free(hb);
    return kl / 2.0;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <original.RAW> <decoded.RAW> <width> <height> [bits]\n", argv[0]);
        return 1;
    }

    const char *path_a = argv[1], *path_b = argv[2];
    int w = atoi(argv[3]), h = atoi(argv[4]);
    int bits = (argc > 5) ? atoi(argv[5]) : 14;
    size_t n = (size_t)w * h;
    size_t expected = n * sizeof(uint16_t);
    int max_val = (1 << bits) - 1;

    FILE *fa = fopen(path_a, "rb");
    FILE *fb = fopen(path_b, "rb");
    if (!fa || !fb) { fprintf(stderr, "Cannot open files\n"); return 1; }

    uint16_t *a = (uint16_t *)malloc(expected);
    uint16_t *b = (uint16_t *)malloc(expected);
    fread(a, 2, n, fa); fread(b, 2, n, fb);
    fclose(fa); fclose(fb);

    printf("=== Comprehensive Quality Comparison ===\n");
    printf("Original: %s\n", path_a);
    printf("Decoded:  %s\n", path_b);
    printf("Size: %dx%d, %d-bit\n\n", w, h, bits);

    /* Global metrics */
    double psnr = compute_psnr(a, b, n, bits);
    double ssim = compute_ssim(a, b, w, h, bits);
    double kl_div = histogram_divergence(a, b, n, bits);

    printf("Global Metrics:\n");
    printf("  PSNR:           %.2f dB\n", psnr);
    printf("  SSIM:           %.6f\n", ssim);
    printf("  Histogram KL:   %.6f bits\n", kl_div);

    /* Error statistics */
    double sse = 0;
    int64_t max_err = 0, sum_err = 0;
    size_t diff_count = 0;
    int err_hist[1024] = {0};  /* error magnitude histogram */

    for (size_t i = 0; i < n; i++) {
        int64_t d = (int64_t)b[i] - (int64_t)a[i];
        sse += (double)d * d;
        sum_err += d;
        if (labs(d) > max_err) max_err = labs(d);
        if (d != 0) diff_count++;
        int bin = (int)(labs(d));
        if (bin > 1023) bin = 1023;
        err_hist[bin]++;
    }

    printf("\nError Distribution:\n");
    printf("  Mean error:     %.4f DN (bias)\n", (double)sum_err / n);
    printf("  RMSE:           %.2f DN\n", sqrt(sse / n));
    printf("  Max error:      %ld DN\n", (long)max_err);
    printf("  Changed pixels: %.1f%%\n", 100.0 * diff_count / n);
    printf("  Within ±1 DN:   %.1f%%\n", 100.0 * (err_hist[0] + err_hist[1]) / n);
    printf("  Within ±5 DN:   %.1f%%\n",
           100.0 * (err_hist[0]+err_hist[1]+err_hist[2]+err_hist[3]+err_hist[4]+err_hist[5]) / n);

    /* Noise statistics preservation */
    double noise_a = estimate_noise(a, w, h);
    double noise_b = estimate_noise(b, w, h);

    printf("\nNoise Preservation:\n");
    printf("  Original noise σ: %.2f DN\n", noise_a);
    printf("  Decoded noise σ:  %.2f DN\n", noise_b);
    printf("  Ratio:            %.2f (1.0 = perfect preservation)\n",
           noise_a > 0 ? noise_b / noise_a : 0);

    /* Per-region analysis */
    double dark_sse = 0, mid_sse = 0, bright_sse = 0;
    int dark_n = 0, mid_n = 0, bright_n = 0;
    int dark_thresh = max_val / 4;
    int bright_thresh = max_val * 3 / 4;

    for (size_t i = 0; i < n; i++) {
        double d = (double)b[i] - (double)a[i];
        double dsq = d * d;
        if (a[i] < dark_thresh) { dark_sse += dsq; dark_n++; }
        else if (a[i] > bright_thresh) { bright_sse += dsq; bright_n++; }
        else { mid_sse += dsq; mid_n++; }
    }

    printf("\nPer-Region PSNR:\n");
    if (dark_n > 0) {
        double dark_mse = dark_sse / dark_n;
        printf("  Dark  (<25%%):  %.2f dB  (%d pixels, %.0f%% of image)\n",
               dark_mse > 0 ? 10*log10((double)max_val*max_val/dark_mse) : 999.0,
               dark_n, 100.0*dark_n/n);
    }
    if (mid_n > 0) {
        double mid_mse = mid_sse / mid_n;
        printf("  Mid   (25-75%%): %.2f dB  (%d pixels, %.0f%% of image)\n",
               mid_mse > 0 ? 10*log10((double)max_val*max_val/mid_mse) : 999.0,
               mid_n, 100.0*mid_n/n);
    }
    if (bright_n > 0) {
        double bright_mse = bright_sse / bright_n;
        printf("  Bright (>75%%): %.2f dB  (%d pixels, %.0f%% of image)\n",
               bright_mse > 0 ? 10*log10((double)max_val*max_val/bright_mse) : 999.0,
               bright_n, 100.0*bright_n/n);
    }

    free(a); free(b);
    return 0;
}
