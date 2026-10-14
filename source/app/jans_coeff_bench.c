#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ans_joint.h"

enum { LOCAL_RUN_CLASSES = 10, LOCAL_MAG_CLASSES = 16 };

static const int local_run_class_min[LOCAL_RUN_CLASSES] =
    {0, 1, 2, 3, 4, 8, 16, 32, 64, 128};
static const int local_run_class_bits[LOCAL_RUN_CLASSES] =
    {0, 0, 0, 0, 2, 3, 4, 5, 6, 7};
static const int local_mag_class_min[LOCAL_MAG_CLASSES] =
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 1024};
static const int local_mag_class_bits[LOCAL_MAG_CLASSES] =
    {0, 0, 0, 0, 0, 0, 0, 0, 3, 4, 5, 6, 7, 8, 9, 10};

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int cmp_double(const void *a, const void *b)
{
    const double da = *(const double *)a;
    const double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void print_summary(const char *name, double *values, int n)
{
    if (!name || !values || n <= 0) return;
    qsort(values, (size_t)n, sizeof(values[0]), cmp_double);
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += values[i];
    int p25 = n / 4;
    int p50 = n / 2;
    int p75 = (3 * n) / 4;
    int p95 = (int)((double)(n - 1) * 0.95 + 0.5);
    if (p95 < 0) p95 = 0;
    if (p95 >= n) p95 = n - 1;
    printf("# jans_coeff_bench_ms %s n=%d mean=%.3f min=%.3f p25=%.3f median=%.3f p75=%.3f p95=%.3f max=%.3f\n",
           name, n, sum / (double)n, values[0], values[p25], values[p50],
           values[p75], values[p95], values[n - 1]);
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !out) return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < 0 || v > 0x7fffffffL) return -1;
    *out = (int)v;
    return 0;
}

static int read_coeffs(const char *path, int32_t *dst, size_t count)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    size_t n = fread(dst, sizeof(dst[0]), count, f);
    int err = ferror(f);
    fclose(f);
    if (err || n != count) {
        fprintf(stderr, "read %s failed: got %zu coeffs, expected %zu\n",
                path, n, count);
        return -1;
    }
    return 0;
}

static int local_run_class(int run)
{
    for (int c = LOCAL_RUN_CLASSES - 1; c >= 0; c--) {
        if (run >= local_run_class_min[c]) return c;
    }
    return 0;
}

static int local_mag_class(int mag)
{
    for (int c = LOCAL_MAG_CLASSES - 1; c >= 0; c--) {
        if (mag >= local_mag_class_min[c]) return c;
    }
    return 0;
}

static void print_coeff_stats(const int32_t *coeffs, int width, int height)
{
    uint64_t zeros = 0;
    uint64_t nonzeros = 0;
    uint64_t zero_run_tokens = 0;
    uint64_t nonzero_tokens = 0;
    uint64_t resid_bits = 0;
    uint64_t run_hist[LOCAL_RUN_CLASSES] = {0};
    uint64_t mag_hist[LOCAL_MAG_CLASSES] = {0};
    int max_mag = 0;
    int ge2048 = 0;
    int run = 0;

    for (int row = 0; row < height; row++) {
        const int32_t *p = coeffs + (size_t)row * (size_t)width;
        for (int col = 0; col < width; col++) {
            int32_t val = p[col];
            if (val == 0) {
                zeros++;
                run++;
                continue;
            }

            nonzeros++;
            int mag = (val < 0) ? (val == INT32_MIN ? INT32_MAX : -val) : val;
            if (mag > max_mag) max_mag = mag;
            if (mag >= 2048) ge2048++;

            while (run >= 256) {
                int rc = local_run_class(255);
                run_hist[rc]++;
                resid_bits += (uint64_t)local_run_class_bits[rc];
                zero_run_tokens++;
                run -= 255;
            }

            int rc = local_run_class(run);
            int mc = local_mag_class(mag);
            run_hist[rc]++;
            mag_hist[mc]++;
            resid_bits += (uint64_t)local_run_class_bits[rc] +
                          (uint64_t)local_mag_class_bits[mc] + 1u;
            nonzero_tokens++;
            run = 0;
        }

        while (run > 0) {
            int actual = (run > 255) ? 255 : run;
            int rc = local_run_class(actual);
            run_hist[rc]++;
            resid_bits += (uint64_t)local_run_class_bits[rc];
            zero_run_tokens++;
            run -= actual;
        }
    }

    uint64_t coeff_count = (uint64_t)(size_t)width * (uint64_t)(size_t)height;
    uint64_t tokens = nonzero_tokens + zero_run_tokens;
    double nonzero_pct = coeff_count ? (100.0 * (double)nonzeros / (double)coeff_count) : 0.0;
    printf("# jans_coeff_stats coeffs=%llu zeros=%llu nonzeros=%llu nonzero_pct=%.3f tokens=%llu nonzero_tokens=%llu zero_run_tokens=%llu resid_bits=%llu max_mag=%d mag_ge2048=%d\n",
           (unsigned long long)coeff_count,
           (unsigned long long)zeros,
           (unsigned long long)nonzeros,
           nonzero_pct,
           (unsigned long long)tokens,
           (unsigned long long)nonzero_tokens,
           (unsigned long long)zero_run_tokens,
           (unsigned long long)resid_bits,
           max_mag,
           ge2048);
    printf("# jans_coeff_run_hist");
    for (int c = 0; c < LOCAL_RUN_CLASSES; c++) {
        printf(" c%d=%llu", c, (unsigned long long)run_hist[c]);
    }
    printf("\n");
    printf("# jans_coeff_mag_hist");
    for (int c = 0; c < LOCAL_MAG_CLASSES; c++) {
        printf(" c%d=%llu", c, (unsigned long long)mag_hist[c]);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 5 || argc > 7) {
        fprintf(stderr,
                "usage: %s <coeff.s32> <width> <height> <iters> [stripe_rows] [defer_rans]\n",
                argv[0]);
        return 2;
    }

    const char *path = argv[1];
    int width = 0, height = 0, iters = 0, stripe_rows = 0, defer_rans = 0;
    if (parse_int(argv[2], &width) || parse_int(argv[3], &height) ||
        parse_int(argv[4], &iters) || width <= 0 || height <= 0 || iters <= 0) {
        fprintf(stderr, "invalid width/height/iters\n");
        return 2;
    }
    if (argc >= 6 && parse_int(argv[5], &stripe_rows)) {
        fprintf(stderr, "invalid stripe_rows\n");
        return 2;
    }
    if (argc >= 7 && parse_int(argv[6], &defer_rans)) {
        fprintf(stderr, "invalid defer_rans\n");
        return 2;
    }

    size_t coeff_count = (size_t)width * (size_t)height;
    if (coeff_count / (size_t)width != (size_t)height) {
        fprintf(stderr, "coefficient dimensions overflow\n");
        return 2;
    }

    int32_t *coeffs = (int32_t *)malloc(coeff_count * sizeof(coeffs[0]));
    if (!coeffs) {
        fprintf(stderr, "coeff allocation failed\n");
        return 1;
    }
    if (read_coeffs(path, coeffs, coeff_count) != 0) {
        free(coeffs);
        return 1;
    }
    print_coeff_stats(coeffs, width, height);

    size_t max_coeffs = coeff_count;
    if (stripe_rows > 0 && !defer_rans) {
        size_t stripe_coeffs = (size_t)width * (size_t)stripe_rows;
        if (stripe_coeffs > 0 && stripe_coeffs < max_coeffs) max_coeffs = stripe_coeffs;
    }

    JANS_INLINE_STATE *st = jans_inline_create(max_coeffs);
    if (!st) {
        fprintf(stderr, "jans_inline_create failed\n");
        free(coeffs);
        return 1;
    }
    jans_inline_set_stripe_rows(st, stripe_rows);
    jans_inline_set_defer_rans(st, defer_rans ? 1 : 0);

    size_t out_cap = coeff_count * 6 + (size_t)height * 16 + 1048576;
    uint8_t *out = (uint8_t *)malloc(out_cap);
    double *row_ms = (double *)malloc((size_t)iters * sizeof(row_ms[0]));
    double *final_ms = (double *)malloc((size_t)iters * sizeof(final_ms[0]));
    double *total_ms = (double *)malloc((size_t)iters * sizeof(total_ms[0]));
    if (!out || !row_ms || !final_ms || !total_ms) {
        fprintf(stderr, "bench allocation failed\n");
        free(total_ms);
        free(final_ms);
        free(row_ms);
        free(out);
        jans_inline_destroy(st);
        free(coeffs);
        return 1;
    }

    size_t last_bytes = 0;
    for (int iter = 0; iter < iters; iter++) {
        jans_inline_reset(st);
        double t0 = now_ms();
        for (int row = 0; row < height; row++) {
            jans_inline_row(st, coeffs + (size_t)row * (size_t)width, width);
        }
        double t1 = now_ms();
        int n = jans_inline_finalize(out, out_cap, st);
        double t2 = now_ms();
        if (n < 0) {
            fprintf(stderr, "jans_inline_finalize failed at iter %d\n", iter);
            free(total_ms);
            free(final_ms);
            free(row_ms);
            free(out);
            jans_inline_destroy(st);
            free(coeffs);
            return 1;
        }
        last_bytes = (size_t)n;
        row_ms[iter] = t1 - t0;
        final_ms[iter] = t2 - t1;
        total_ms[iter] = t2 - t0;
    }

    printf("# jans_coeff_bench coeff=%s width=%d height=%d iters=%d stripe_rows=%d defer_rans=%d bytes=%zu\n",
           path, width, height, iters, stripe_rows, defer_rans ? 1 : 0,
           last_bytes);
    print_summary("rows", row_ms, iters);
    print_summary("finalize", final_ms, iters);
    print_summary("total", total_ms, iters);

    free(total_ms);
    free(final_ms);
    free(row_ms);
    free(out);
    jans_inline_destroy(st);
    free(coeffs);
    return 0;
}
