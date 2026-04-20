/*! @file fuzz_ans.c
 *  @brief libFuzzer entry point for ANS decode functions.
 *
 *  Tests ans_decode_band and jans_decode_band with arbitrary input data
 *  to find crashes, buffer overflows, and undefined behavior.
 *
 *  Build: clang -fsanitize=fuzzer,address -I... -o fuzz_ans fuzz_ans.c ans.c ans_joint.c
 *  Run:   ./fuzz_ans corpus/ -max_len=65536
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

#include "ans.h"
#include "ans_joint.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 16) return 0;

    /* Use first byte to select which function to fuzz */
    int mode = data[0] % 3;
    const uint8_t *payload = data + 1;
    size_t payload_size = size - 1;

    /* Small fixed output buffer — decoder should handle any dimensions safely */
    int width = 64, height = 48;
    int pitch = width * sizeof(int32_t);
    int32_t *output = (int32_t *)calloc(width * height, sizeof(int32_t));
    if (!output) return 0;

    if (mode == 0) {
        /* Fuzz ans_decode_band with pre-built (potentially corrupt) tables */
        ANS_BAND_CTX ctx;
        memset(&ctx, 0, sizeof(ctx));

        /* Try to deserialize tables from the fuzzer input */
        size_t needed = ANS_NUM_SYMBOLS * 2 * 2;
        if (payload_size >= needed) {
            ans_deserialize_tables(&ctx, payload, payload_size);

            /* Decode with the remaining data */
            const uint8_t *coded = payload + needed;
            size_t coded_size = payload_size - needed;
            ans_decode_band(coded, coded_size, &ctx, output, width, height, pitch);
        }
    }
    else if (mode == 1) {
        /* Fuzz jans_decode_band directly */
        jans_decode_band(payload, payload_size, output, width, height, pitch);
    }
    else {
        /* Fuzz ans_decode_band with raw data (no table deserialization) */
        ANS_BAND_CTX ctx;
        memset(&ctx, 0, sizeof(ctx));
        /* Set uniform frequency table */
        for (int i = 0; i < ANS_NUM_SYMBOLS; i++)
            ctx.run_table.freq[i] = ctx.mag_table.freq[i] = 1;
        ctx.initialized = 1;
        ans_decode_band(payload, payload_size, &ctx, output, width, height, pitch);
    }

    free(output);
    return 0;
}

/* Non-fuzzer main for manual testing */
#ifndef __AFL_COMPILER
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <input_file>\n", argv[0]);
        printf("Or build with -fsanitize=fuzzer for libFuzzer mode.\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);
    LLVMFuzzerTestOneInput(data, size);
    free(data);
    printf("OK\n");
    return 0;
}
#endif
#endif
