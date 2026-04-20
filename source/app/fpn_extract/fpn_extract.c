/*! @file fpn_extract.c
 *
 *  @brief Fixed-Pattern Noise extraction tool.
 *
 *  Averages multiple raw Bayer frames to cancel random noise, revealing
 *  the deterministic fixed-pattern noise (FPN). Fits the FPN with a 2D
 *  polynomial model and outputs a JSON calibration file.
 *
 *  Usage: fpn_extract <width> <height> <bits> <output.json> <input1.RAW> [input2.RAW ...]
 *
 *  The input .RAW files should be decoded from GPR files first:
 *    gpr_tools -i frame001.GPR -o frame001.RAW
 *
 *  For best results, use 50+ frames from the same sensor at the same
 *  settings (e.g., a night timelapse sequence).
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define MAX_POLY_ORDER 4
#define NUM_POLY_TERMS(order) (((order)+1)*((order)+2)/2)  /* 15 for order 4 */

/* 2D polynomial basis function: x^i * y^j for all i+j <= order */
static int eval_poly_basis(double x, double y, int order, double *basis)
{
    int idx = 0;
    for (int total = 0; total <= order; total++)
        for (int i = total; i >= 0; i--)
        {
            int j = total - i;
            basis[idx++] = pow(x, i) * pow(y, j);
        }
    return idx;
}

/* Least-squares polynomial fit using normal equations (A^T A x = A^T b)
   Fits a 2D polynomial of given order to (x,y,value) data points.
   Uses subsampling for large images. */
static void fit_poly_2d(const double *data, int w, int h, int order, double *coeffs)
{
    int n_terms = NUM_POLY_TERMS(order);
    int max_samples = 50000;
    int step = (w * h > max_samples) ? (w * h / max_samples) : 1;

    /* Allocate normal equation matrices: A^T A (n×n) and A^T b (n×1) */
    double *ATA = (double *)calloc(n_terms * n_terms, sizeof(double));
    double *ATb = (double *)calloc(n_terms, sizeof(double));
    double *basis = (double *)malloc(n_terms * sizeof(double));

    int sample_idx = 0;
    for (int row = 0; row < h; row++)
    {
        double y = (2.0 * row / (h - 1)) - 1.0;  /* Normalize to [-1, 1] */
        for (int col = 0; col < w; col++)
        {
            if (sample_idx % step != 0) { sample_idx++; continue; }
            sample_idx++;

            double x = (2.0 * col / (w - 1)) - 1.0;
            double val = data[row * w + col];

            eval_poly_basis(x, y, order, basis);

            /* Accumulate A^T A and A^T b */
            for (int i = 0; i < n_terms; i++)
            {
                ATb[i] += basis[i] * val;
                for (int j = 0; j <= i; j++)
                {
                    double v = basis[i] * basis[j];
                    ATA[i * n_terms + j] += v;
                    if (i != j) ATA[j * n_terms + i] += v;
                }
            }
        }
    }

    /* Solve via Cholesky-like Gauss elimination (symmetric positive definite) */
    for (int k = 0; k < n_terms; k++)
    {
        double pivot = ATA[k * n_terms + k];
        if (fabs(pivot) < 1e-12) { coeffs[k] = 0; continue; }
        for (int i = k + 1; i < n_terms; i++)
        {
            double factor = ATA[i * n_terms + k] / pivot;
            for (int j = k; j < n_terms; j++)
                ATA[i * n_terms + j] -= factor * ATA[k * n_terms + j];
            ATb[i] -= factor * ATb[k];
        }
    }
    /* Back substitution */
    for (int k = n_terms - 1; k >= 0; k--)
    {
        double sum = ATb[k];
        for (int j = k + 1; j < n_terms; j++)
            sum -= ATA[k * n_terms + j] * coeffs[j];
        coeffs[k] = sum / ATA[k * n_terms + k];
    }

    free(ATA); free(ATb); free(basis);
}

/* Evaluate polynomial at a point */
static double eval_poly_2d(const double *coeffs, int order, double x, double y)
{
    double basis[NUM_POLY_TERMS(MAX_POLY_ORDER)];
    int n = eval_poly_basis(x, y, order, basis);
    double val = 0;
    for (int i = 0; i < n; i++)
        val += coeffs[i] * basis[i];
    return val;
}

int main(int argc, char **argv)
{
    if (argc < 6)
    {
        fprintf(stderr, "Usage: %s <width> <height> <bits> <output.json> <input1.RAW> [input2.RAW ...]\n", argv[0]);
        fprintf(stderr, "\nExtracts fixed-pattern noise by averaging multiple raw frames.\n");
        fprintf(stderr, "Decode GPR files first: gpr_tools -i frame.GPR -o frame.RAW\n");
        return 1;
    }

    int w = atoi(argv[1]);
    int h = atoi(argv[2]);
    int bits = atoi(argv[3]);
    const char *output_path = argv[4];
    int num_frames = argc - 5;
    size_t npixels = (size_t)w * h;
    size_t expected_size = npixels * sizeof(uint16_t);

    printf("FPN Extraction: %dx%d, %d-bit, %d frames\n", w, h, bits, num_frames);

    /* Allocate per-pixel float64 accumulator (4 Bayer channels) */
    double *accum = (double *)calloc(npixels, sizeof(double));
    if (!accum) { fprintf(stderr, "Out of memory\n"); return 1; }

    /* Accumulate frames */
    uint16_t *raw = (uint16_t *)malloc(expected_size);
    int valid_frames = 0;

    for (int f = 0; f < num_frames; f++)
    {
        const char *path = argv[5 + f];
        FILE *fp = fopen(path, "rb");
        if (!fp) { fprintf(stderr, "Cannot open: %s\n", path); continue; }

        fseek(fp, 0, SEEK_END);
        size_t fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (fsize != expected_size)
        {
            fprintf(stderr, "Size mismatch: %s (expected %zu, got %zu)\n", path, expected_size, fsize);
            fclose(fp);
            continue;
        }

        fread(raw, sizeof(uint16_t), npixels, fp);
        fclose(fp);

        for (size_t i = 0; i < npixels; i++)
            accum[i] += (double)raw[i];

        valid_frames++;
        if (valid_frames % 10 == 0)
            printf("  Processed %d/%d frames\n", valid_frames, num_frames);
    }

    free(raw);
    printf("  Averaged %d valid frames\n", valid_frames);

    if (valid_frames == 0) { free(accum); return 1; }

    /* Compute per-pixel average */
    for (size_t i = 0; i < npixels; i++)
        accum[i] /= valid_frames;

    /* Compute per-channel mean and subtract (FPN is relative to channel mean) */
    double chan_mean[4] = {0};
    int chan_count[4] = {0};
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            int ch = (row & 1) * 2 + (col & 1);
            chan_mean[ch] += accum[row * w + col];
            chan_count[ch]++;
        }
    for (int ch = 0; ch < 4; ch++)
        if (chan_count[ch] > 0) chan_mean[ch] /= chan_count[ch];

    printf("  Channel means: R=%.1f Gr=%.1f Gb=%.1f B=%.1f\n",
           chan_mean[0], chan_mean[1], chan_mean[2], chan_mean[3]);

    /* Extract per-channel FPN (subtract mean) */
    int half_w = w / 2, half_h = h / 2;
    int half_size = half_w * half_h;
    double *fpn[4];
    for (int ch = 0; ch < 4; ch++)
        fpn[ch] = (double *)calloc(half_size, sizeof(double));

    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
        {
            int ch = (row & 1) * 2 + (col & 1);
            int hr = row / 2, hc = col / 2;
            fpn[ch][hr * half_w + hc] = accum[row * w + col] - chan_mean[ch];
        }

    free(accum);

    /* Compute FPN sigma per channel */
    double fpn_sigma[4] = {0};
    for (int ch = 0; ch < 4; ch++)
    {
        double sum_sq = 0;
        for (int i = 0; i < half_size; i++)
            sum_sq += fpn[ch][i] * fpn[ch][i];
        fpn_sigma[ch] = sqrt(sum_sq / half_size);
    }
    printf("  FPN sigma: R=%.2f Gr=%.2f Gb=%.2f B=%.2f\n",
           fpn_sigma[0], fpn_sigma[1], fpn_sigma[2], fpn_sigma[3]);

    /* Fit 2D polynomial to each channel's FPN */
    int poly_order = MAX_POLY_ORDER;
    int n_terms = NUM_POLY_TERMS(poly_order);
    double poly_coeffs[4][NUM_POLY_TERMS(MAX_POLY_ORDER)];

    for (int ch = 0; ch < 4; ch++)
    {
        fit_poly_2d(fpn[ch], half_w, half_h, poly_order, poly_coeffs[ch]);
        printf("  Channel %d: poly[0]=%.4f (DC offset)\n", ch, poly_coeffs[ch][0]);
    }

    /* Subtract polynomial from FPN to get residual */
    double *residual[4];
    for (int ch = 0; ch < 4; ch++)
    {
        residual[ch] = (double *)malloc(half_size * sizeof(double));
        for (int row = 0; row < half_h; row++)
            for (int col = 0; col < half_w; col++)
            {
                double x = (2.0 * col / (half_w - 1)) - 1.0;
                double y = (2.0 * row / (half_h - 1)) - 1.0;
                double fitted = eval_poly_2d(poly_coeffs[ch], poly_order, x, y);
                residual[ch][row * half_w + col] = fpn[ch][row * half_w + col] - fitted;
            }
    }
    for (int ch = 0; ch < 4; ch++) free(fpn[ch]);

    /* Compute per-row and per-column mean offsets from residual (banding correction) */
    double *row_offsets[4], *col_offsets[4];
    for (int ch = 0; ch < 4; ch++)
    {
        row_offsets[ch] = (double *)calloc(half_h, sizeof(double));
        col_offsets[ch] = (double *)calloc(half_w, sizeof(double));

        /* Per-row means */
        for (int row = 0; row < half_h; row++)
        {
            double sum = 0;
            for (int col = 0; col < half_w; col++)
                sum += residual[ch][row * half_w + col];
            row_offsets[ch][row] = sum / half_w;
        }

        /* Per-column means */
        for (int col = 0; col < half_w; col++)
        {
            double sum = 0;
            for (int row = 0; row < half_h; row++)
                sum += residual[ch][row * half_w + col];
            col_offsets[ch][col] = sum / half_h;
        }
    }

    /* Compute residual sigma after polynomial + row/col correction */
    double residual_sigma[4] = {0};
    double residual_sigma_rc[4] = {0};
    for (int ch = 0; ch < 4; ch++)
    {
        double sum_sq = 0, sum_sq_rc = 0;
        for (int row = 0; row < half_h; row++)
            for (int col = 0; col < half_w; col++)
            {
                double r = residual[ch][row * half_w + col];
                sum_sq += r * r;
                double r_rc = r - row_offsets[ch][row] - col_offsets[ch][col];
                sum_sq_rc += r_rc * r_rc;
            }
        residual_sigma[ch] = sqrt(sum_sq / half_size);
        residual_sigma_rc[ch] = sqrt(sum_sq_rc / half_size);
    }
    for (int ch = 0; ch < 4; ch++) free(residual[ch]);

    printf("  Residual sigma (poly only): R=%.2f Gr=%.2f Gb=%.2f B=%.2f\n",
           residual_sigma[0], residual_sigma[1], residual_sigma[2], residual_sigma[3]);
    printf("  Residual sigma (poly+row/col): R=%.2f Gr=%.2f Gb=%.2f B=%.2f\n",
           residual_sigma_rc[0], residual_sigma_rc[1], residual_sigma_rc[2], residual_sigma_rc[3]);
    printf("  Poly captured: R=%.0f%% Gr=%.0f%% Gb=%.0f%% B=%.0f%%\n",
           (1 - residual_sigma[0]/fpn_sigma[0]) * 100,
           (1 - residual_sigma[1]/fpn_sigma[1]) * 100,
           (1 - residual_sigma[2]/fpn_sigma[2]) * 100,
           (1 - residual_sigma[3]/fpn_sigma[3]) * 100);
    printf("  Poly+Row/Col captured: R=%.0f%% Gr=%.0f%% Gb=%.0f%% B=%.0f%%\n",
           (1 - residual_sigma_rc[0]/fpn_sigma[0]) * 100,
           (1 - residual_sigma_rc[1]/fpn_sigma[1]) * 100,
           (1 - residual_sigma_rc[2]/fpn_sigma[2]) * 100,
           (1 - residual_sigma_rc[3]/fpn_sigma[3]) * 100);

    /* Generate seed from polynomial coefficients */
    uint32_t seed = 0x55AA55AA;
    for (int ch = 0; ch < 4; ch++)
        for (int i = 0; i < n_terms; i++)
            seed ^= (uint32_t)(poly_coeffs[ch][i] * 1000000.0) * 2654435761u;

    /* Write JSON output */
    FILE *out = fopen(output_path, "w");
    if (!out) { fprintf(stderr, "Cannot write: %s\n", output_path); return 1; }

    fprintf(out, "{\n");
    fprintf(out, "  \"width\": %d,\n", w);
    fprintf(out, "  \"height\": %d,\n", h);
    fprintf(out, "  \"bits\": %d,\n", bits);
    fprintf(out, "  \"num_frames_averaged\": %d,\n", valid_frames);
    fprintf(out, "  \"fpn_poly_order\": %d,\n", poly_order);
    fprintf(out, "  \"channel_means\": [%.4f, %.4f, %.4f, %.4f],\n",
            chan_mean[0], chan_mean[1], chan_mean[2], chan_mean[3]);
    fprintf(out, "  \"fpn_sigma\": [%.4f, %.4f, %.4f, %.4f],\n",
            fpn_sigma[0], fpn_sigma[1], fpn_sigma[2], fpn_sigma[3]);
    fprintf(out, "  \"residual_sigma\": [%.4f, %.4f, %.4f, %.4f],\n",
            residual_sigma[0], residual_sigma[1], residual_sigma[2], residual_sigma[3]);
    fprintf(out, "  \"fpn_seed\": %u,\n", seed);

    const char *ch_names[] = {"fpn_poly_R", "fpn_poly_Gr", "fpn_poly_Gb", "fpn_poly_B"};
    for (int ch = 0; ch < 4; ch++)
    {
        fprintf(out, "  \"%s\": [", ch_names[ch]);
        for (int i = 0; i < n_terms; i++)
            fprintf(out, "%s%.8f", i > 0 ? ", " : "", poly_coeffs[ch][i]);
        fprintf(out, "],\n");
    }

    /* Write per-row offsets */
    const char *row_names[] = {"row_offsets_R", "row_offsets_Gr", "row_offsets_Gb", "row_offsets_B"};
    for (int ch = 0; ch < 4; ch++)
    {
        fprintf(out, "  \"%s\": [", row_names[ch]);
        for (int i = 0; i < half_h; i++)
            fprintf(out, "%s%.4f", i > 0 ? "," : "", row_offsets[ch][i]);
        fprintf(out, "],\n");
    }

    /* Write per-column offsets */
    const char *col_names[] = {"col_offsets_R", "col_offsets_Gr", "col_offsets_Gb", "col_offsets_B"};
    for (int ch = 0; ch < 4; ch++)
    {
        fprintf(out, "  \"%s\": [", col_names[ch]);
        for (int i = 0; i < half_w; i++)
            fprintf(out, "%s%.4f", i > 0 ? "," : "", col_offsets[ch][i]);
        fprintf(out, "]%s\n", ch < 3 ? "," : "");
    }

    for (int ch = 0; ch < 4; ch++) { free(row_offsets[ch]); free(col_offsets[ch]); }

    fprintf(out, "}\n");
    fclose(out);

    printf("\nCalibration written to: %s\n", output_path);
    printf("  %d polynomial terms per channel (%d total)\n", n_terms, n_terms * 4);
    return 0;
}
