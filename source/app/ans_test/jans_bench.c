/*! @file jans_bench.c
 *  @brief Benchmark and correctness test for two-pass vs one-pass ANS decode.
 *
 *  Tests jans_decode_band_x4() (two-pass) against jans_decode_band_x4_onepass()
 *  to verify identical output and measure performance difference.
 *
 *  Build: cc -std=c99 -O2 -I../../lib/vc5_common ../../lib/vc5_common/ans_joint.c jans_bench.c -o jans_bench -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "ans_joint.h"

/* Simple PRNG (deterministic for reproducibility) */
static unsigned int g_seed = 12345;
static int prng_next(void) {
    g_seed = g_seed * 1103515245 + 12345;
    return (int)((g_seed >> 16) & 0x7FFF);
}

/* Generate wavelet-like coefficient data with known distribution */
static void generate_test_band(int32_t *data, int width, int height, int pitch,
                               double zero_fraction, double sigma)
{
    int pitch_elems = pitch / (int)sizeof(int32_t);

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            double u = (double)prng_next() / 32768.0;

            if (u < zero_fraction) {
                data[row * pitch_elems + col] = 0;
            } else {
                double v = (double)prng_next() / 32768.0 - 0.5;
                int32_t val = (int32_t)(v * sigma * 2);
                if (val == 0) val = 1;
                data[row * pitch_elems + col] = val;
            }
        }
    }
}

/* Get wall clock time in seconds */
static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Compare two output buffers, return number of mismatches */
static int compare_output(const int32_t *a, const int32_t *b,
                          int width, int height, int pitch)
{
    int pitch_elems = pitch / (int)sizeof(int32_t);
    int errors = 0;
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            int idx = row * pitch_elems + col;
            if (a[idx] != b[idx]) {
                if (errors < 10) {
                    printf("  MISMATCH at [%d,%d]: onepass=%d twopass=%d\n",
                           row, col, a[idx], b[idx]);
                }
                errors++;
            }
        }
    }
    return errors;
}

typedef struct {
    const char *name;
    double zero_frac;
    double sigma;
    int width;
    int height;
} TEST_CONFIG;

int main(int argc, char *argv[])
{
    printf("=== Joint ANS Two-Pass Decode Benchmark ===\n\n");

    int num_iters = 50;
    if (argc > 1) num_iters = atoi(argv[1]);
    if (num_iters < 1) num_iters = 1;

    TEST_CONFIG tests[] = {
        /* Simulate Z8 quality bands (the target workload) */
        {"Z8 HH band (2028x1520)",   0.50, 50.0, 2028, 1520},
        {"Z8 HL band (2028x1520)",   0.55, 40.0, 2028, 1520},
        {"Z8 LH band (2028x1520)",   0.55, 40.0, 2028, 1520},
        /* Smaller bands for quick sanity */
        {"Small sparse (256x256)",    0.92, 10.0, 256,  256},
        {"Small dense (256x256)",     0.30, 80.0, 256,  256},
        /* Edge cases */
        {"Tall narrow (32x4096)",     0.70, 30.0, 32,   4096},
        {"Wide short (4096x32)",      0.70, 30.0, 4096, 32},
        /* Pitch != width (extra columns for alignment) */
        {"Pitch-padded (1000x750)",   0.80, 20.0, 1000, 750},
    };
    int num_tests = (int)(sizeof(tests) / sizeof(tests[0]));

    printf("%-30s %8s %8s %8s %8s %8s\n",
           "Test", "Tokens", "1Pass", "2Pass", "Speedup", "Errors");
    printf("%-30s %8s %8s %8s %8s %8s\n",
           "", "", "(ms)", "(ms)", "", "");
    printf("--------------------------------------------------------------------------\n");

    for (int tc = 0; tc < num_tests; tc++) {
        int width = tests[tc].width;
        int height = tests[tc].height;
        /* For the pitch-padded test, add 64 extra columns */
        int alloc_width = width;
        if (tc == num_tests - 1) alloc_width = width + 64;
        int pitch = alloc_width * (int)sizeof(int32_t);

        size_t band_bytes = (size_t)alloc_width * (size_t)height * sizeof(int32_t);
        int32_t *band_orig   = (int32_t *)malloc(band_bytes);
        int32_t *decoded_1p  = (int32_t *)malloc(band_bytes);
        int32_t *decoded_2p  = (int32_t *)malloc(band_bytes);
        size_t out_cap = band_bytes * 2;
        uint8_t *compressed = (uint8_t *)malloc(out_cap);

        if (!band_orig || !decoded_1p || !decoded_2p || !compressed) {
            printf("malloc failed for test %d\n", tc);
            return 1;
        }

        /* Generate test data */
        g_seed = 12345 + tc * 7;
        generate_test_band(band_orig, width, height, pitch,
                          tests[tc].zero_frac, tests[tc].sigma);

        /* Encode with x4 interleave */
        int enc_size = jans_encode_band_x4(compressed, out_cap,
                                           band_orig, width, height, pitch);
        if (enc_size < 0) {
            printf("%-30s ENCODE FAILED\n", tests[tc].name);
            free(band_orig); free(decoded_1p); free(decoded_2p); free(compressed);
            continue;
        }

        /* Extract token count from header for display */
        int token_count = (compressed[0]<<24)|(compressed[1]<<16)|(compressed[2]<<8)|compressed[3];

        /* Correctness: decode both ways and compare */
        int rc1 = jans_decode_band_x4_onepass(compressed, enc_size,
                                              decoded_1p, width, height, pitch);
        int rc2 = jans_decode_band_x4(compressed, enc_size,
                                      decoded_2p, width, height, pitch);
        if (rc1 != 0) {
            printf("%-30s ONEPASS DECODE FAILED\n", tests[tc].name);
            free(band_orig); free(decoded_1p); free(decoded_2p); free(compressed);
            continue;
        }
        if (rc2 != 0) {
            printf("%-30s TWOPASS DECODE FAILED\n", tests[tc].name);
            free(band_orig); free(decoded_1p); free(decoded_2p); free(compressed);
            continue;
        }

        /* Compare twopass output against onepass (the reference) */
        int errors = compare_output(decoded_1p, decoded_2p, width, height, pitch);

        /* Also verify onepass matches the original */
        int orig_errors = compare_output(band_orig, decoded_1p, width, height, pitch);
        if (orig_errors > 0) {
            printf("  WARNING: onepass has %d mismatches vs original\n", orig_errors);
        }

        /* Benchmark: onepass */
        double t1_start = get_time();
        for (int i = 0; i < num_iters; i++) {
            jans_decode_band_x4_onepass(compressed, enc_size,
                                       decoded_1p, width, height, pitch);
        }
        double t1_end = get_time();
        double ms_1p = (t1_end - t1_start) / num_iters * 1000.0;

        /* Benchmark: twopass */
        double t2_start = get_time();
        for (int i = 0; i < num_iters; i++) {
            jans_decode_band_x4(compressed, enc_size,
                                decoded_2p, width, height, pitch);
        }
        double t2_end = get_time();
        double ms_2p = (t2_end - t2_start) / num_iters * 1000.0;

        double speedup = ms_1p / ms_2p;

        printf("%-30s %8d %7.2f %7.2f %7.2fx %6d\n",
               tests[tc].name, token_count, ms_1p, ms_2p, speedup, errors);

        free(band_orig);
        free(decoded_1p);
        free(decoded_2p);
        free(compressed);
    }

    printf("\nIterations per test: %d\n", num_iters);
    printf("Done.\n");
    return 0;
}
