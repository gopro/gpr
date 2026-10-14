/* Verify inline jANS serializes the full residual-bit tail.
 *
 * The register-hoisted inline tokenizer can finish a stripe/blob with more
 * than one pending byte in BITBUF.accum. The public finalizer must flush all
 * of those bytes before jans_decode_band_x4 reads the residual stream.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ans_joint.h"

static int run_case(const char *name, int stripe_rows, int defer_rans,
                    const uint8_t *expected, int expected_size)
{
    const int width = 2;
    const int height = 1;
    const int32_t input[2] = {1024, -1535};
    int32_t decoded[2] = {0, 0};
    uint8_t encoded[4096];

    JANS_INLINE_STATE *state = jans_inline_create((size_t)width * height);
    if (!state) {
        fprintf(stderr, "%s: jans_inline_create failed\n", name);
        return 1;
    }

    jans_inline_set_stripe_rows(state, stripe_rows);
    jans_inline_set_defer_rans(state, defer_rans);
    jans_inline_reset(state);
    jans_inline_row(state, input, width);

    int encoded_size = jans_inline_finalize(encoded, sizeof(encoded), state);
    jans_inline_destroy(state);
    if (encoded_size <= 0) {
        fprintf(stderr, "%s: jans_inline_finalize failed: %d\n", name, encoded_size);
        return 1;
    }
    if (expected && (encoded_size != expected_size ||
                     memcmp(encoded, expected, (size_t)expected_size) != 0)) {
        fprintf(stderr, "%s: encoded bytes differ from split x4 reference: got=%d expected=%d\n",
                name, encoded_size, expected_size);
        return 1;
    }

    int rc = jans_decode_band_x4(encoded, (size_t)encoded_size, decoded,
                                 width, height, width * (int)sizeof(decoded[0]));
    if (rc != 0) {
        fprintf(stderr, "%s: decode failed: %d\n", name, rc);
        return 1;
    }

    if (memcmp(input, decoded, sizeof(input)) != 0) {
        int freq_size = (encoded[4] << 24) | (encoded[5] << 16) |
                        (encoded[6] << 8) | encoded[7];
        int rans_size = (encoded[8] << 24) | (encoded[9] << 16) |
                        (encoded[10] << 8) | encoded[11];
        int resid_size = (encoded[12] << 24) | (encoded[13] << 16) |
                         (encoded[14] << 8) | encoded[15];
        const uint8_t *resid = encoded + 16 + freq_size + rans_size;
        fprintf(stderr, "%s: mismatch input=[%d,%d] decoded=[%d,%d] bytes=%d\n",
                name, input[0], input[1], decoded[0], decoded[1], encoded_size);
        if (encoded[0] != 0xFF) {
            fprintf(stderr, "%s: freq=%d rans=%d resid=%d resid_bytes=%02x %02x %02x %02x\n",
                    name, freq_size, rans_size, resid_size,
                    resid_size > 0 ? resid[0] : 0,
                    resid_size > 1 ? resid[1] : 0,
                    resid_size > 2 ? resid[2] : 0,
                    resid_size > 3 ? resid[3] : 0);
        }
        return 1;
    }

    return 0;
}

int main(void)
{
    int failures = 0;
    uint8_t reference_encoded[4096];
    int reference_size = 0;
    {
        const int width = 2;
        const int height = 1;
        const int32_t input[2] = {1024, -1535};
        int32_t decoded[2] = {0, 0};
        reference_size = jans_encode_band_x4(reference_encoded, sizeof(reference_encoded), input,
                                             width, height,
                                             width * (int)sizeof(input[0]));
        if (reference_size <= 0 ||
            jans_decode_band_x4(reference_encoded, (size_t)reference_size, decoded,
                                width, height,
                                width * (int)sizeof(decoded[0])) != 0 ||
            memcmp(input, decoded, sizeof(input)) != 0) {
            fprintf(stderr, "reference_x4: mismatch input=[%d,%d] decoded=[%d,%d] bytes=%d\n",
                    input[0], input[1], decoded[0], decoded[1], reference_size);
            failures++;
        }
    }
    failures += run_case("single_blob", 0, 0, reference_encoded, reference_size);
    failures += run_case("stripe_immediate", 1, 0, NULL, 0);
    failures += run_case("stripe_deferred", 1, 1, NULL, 0);
    if (failures) return 1;
    printf("test_jans_inline_tail_flush: PASS\n");
    return 0;
}
