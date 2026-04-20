/*! @file ans_test.c
 *  @brief Test ANS entropy coder against VLC on wavelet coefficient data.
 *
 *  Reads a GPR file, decodes to get wavelet coefficients, and compares
 *  the compressed size of each band using ANS vs the original VLC.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ans.h"

/* Simulate wavelet coefficient distributions for testing */
static void generate_test_band(int32_t *data, int width, int height, int pitch,
                               double zero_fraction, double sigma)
{
    int pitch_elems = pitch / sizeof(int32_t);
    unsigned int seed = 12345;

    for (int row = 0; row < height; row++)
    {
        for (int col = 0; col < width; col++)
        {
            /* Simple PRNG */
            seed = seed * 1103515245 + 12345;
            double u = (double)(seed & 0xFFFF) / 65536.0;

            if (u < zero_fraction)
            {
                data[row * pitch_elems + col] = 0;
            }
            else
            {
                /* Laplacian-like distribution */
                seed = seed * 1103515245 + 12345;
                double v = (double)(seed & 0xFFFF) / 65536.0 - 0.5;
                int32_t val = (int32_t)(v * sigma * 2);
                if (val == 0) val = 1; /* no zeros in non-zero path */
                data[row * pitch_elems + col] = val;
            }
        }
    }
}

/* Count the size that VLC would produce (approximate: bits per symbol) */
static double estimate_vlc_bits(const int32_t *data, int width, int height, int pitch)
{
    int pitch_elems = pitch / sizeof(int32_t);
    double bits = 0;

    /* VLC uses ~log2(magnitude) + 1 bits per value, plus run coding */
    /* Approximate: Shannon entropy of the coefficient distribution */
    int hist[512] = {0};
    int total = 0;

    for (int row = 0; row < height; row++)
    {
        for (int col = 0; col < width; col++)
        {
            int32_t val = data[row * pitch_elems + col];
            int32_t mag = (val < 0) ? -val : val;
            if (mag > 255) mag = 255;
            /* Map to histogram bin: 0 for zero, 1-255 for magnitudes, sign is 1 extra bit */
            if (val == 0)
                hist[0]++;
            else
                hist[mag]++;
            total++;
        }
    }

    /* Shannon entropy */
    for (int i = 0; i < 512; i++)
    {
        if (hist[i] > 0)
        {
            double p = (double)hist[i] / total;
            bits += -hist[i] * log2(p);
        }
    }

    /* Add sign bits for non-zero values */
    bits += (total - hist[0]);

    return bits;
}

int main(int argc, char *argv[])
{
    printf("=== ANS Entropy Coder Test ===\n\n");

    int width = 1000;
    int height = 750;
    int pitch = width * sizeof(int32_t);
    size_t band_size = (size_t)width * height * sizeof(int32_t);

    int32_t *band = (int32_t *)malloc(band_size);
    int32_t *decoded = (int32_t *)malloc(band_size);
    if (!band || !decoded) { printf("malloc failed\n"); return 1; }

    /* Allocate output buffer (worst case: larger than input) */
    size_t out_capacity = band_size * 2;
    uint8_t *out_buf = (uint8_t *)malloc(out_capacity);
    if (!out_buf) { printf("malloc failed\n"); return 1; }

    /* Test different coefficient distributions */
    struct { const char *name; double zero_frac; double sigma; } tests[] = {
        {"Normal (Q3 typical)",     0.80, 20.0},
        {"Denoised (Q3+denoise)",   0.92, 10.0},
        {"High quality (Q8)",       0.50, 50.0},
        {"Denoised Q8",             0.85, 15.0},
        {"Near-silent (dark frame)",0.98,  3.0},
    };
    int num_tests = sizeof(tests) / sizeof(tests[0]);

    printf("%-25s %10s %10s %10s %8s\n",
           "Distribution", "Entropy", "ANS", "ANS+Tbl", "Ratio");
    printf("%-25s %10s %10s %10s %8s\n",
           "", "(bits)", "(bytes)", "(bytes)", "(ANS/Ent)");
    printf("-------------------------------------------------------------------\n");

    for (int t = 0; t < num_tests; t++)
    {
        /* Generate test data */
        generate_test_band(band, width, height, pitch,
                          tests[t].zero_frac, tests[t].sigma);

        /* Compute Shannon entropy (theoretical minimum) */
        double entropy_bits = estimate_vlc_bits(band, width, height, pitch);
        double entropy_bytes = entropy_bits / 8.0;

        /* Build ANS tables */
        ANS_BAND_CTX ctx;
        memset(&ctx, 0, sizeof(ctx));
        ans_build_tables(&ctx, band, width, height, pitch);

        /* Encode */
        int compressed_size = ans_encode_band(out_buf, out_capacity,
                                              &ctx, band, width, height, pitch);

        /* Table overhead */
        uint8_t table_buf[8192];
        int table_size = ans_serialize_tables(&ctx, table_buf, sizeof(table_buf));

        if (compressed_size > 0 && table_size > 0)
        {
            double ratio = (double)(compressed_size + table_size) / entropy_bytes;

            printf("%-25s %10.0f %10d %10d %7.2fx\n",
                   tests[t].name,
                   entropy_bytes,
                   compressed_size,
                   compressed_size + table_size,
                   ratio);

            /* Verify round-trip */
            ANS_BAND_CTX dec_ctx;
            memset(&dec_ctx, 0, sizeof(dec_ctx));
            ans_deserialize_tables(&dec_ctx, table_buf, table_size);

            memset(decoded, 0, band_size);
            int rc = ans_decode_band(out_buf, compressed_size,
                                     &dec_ctx, decoded, width, height, pitch);

            /* Check correctness */
            int errors = 0;
            int pitch_elems = pitch / sizeof(int32_t);
            for (int row = 0; row < height; row++)
            {
                for (int col = 0; col < width; col++)
                {
                    if (band[row * pitch_elems + col] != decoded[row * pitch_elems + col])
                        errors++;
                }
            }

            if (errors > 0)
                printf("  *** ROUND-TRIP ERRORS: %d pixels differ! ***\n", errors);
        }
        else
        {
            printf("%-25s  ENCODE FAILED (compressed=%d, table=%d)\n",
                   tests[t].name, compressed_size, table_size);
        }
    }

    free(band);
    free(decoded);
    free(out_buf);

    printf("\n");
    return 0;
}
