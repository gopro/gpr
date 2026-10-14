#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FUSED_ENCODER FUSED_ENCODER;
extern FUSED_ENCODER *gpr_encode_fused_create(int, int, int, int);
extern int gpr_encode_fused_frame(FUSED_ENCODER *, const uint8_t *, size_t,
                                  uint8_t **, size_t *);
extern void gpr_encode_fused_destroy(FUSED_ENCODER *);

/* Link a PASS2_POOL_FORCE=1 encoder so many-core CI also covers this path. */
int main(void)
{
    uint16_t raw[128 * 128];
    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i)
        raw[i] = (uint16_t)((i * 17 + i / 128 * 31) & 16383);
    setenv("FUSED_MULTI_LEVEL", "0", 1);
    setenv("GPR_INCLUDE_LL", "1", 1);
    setenv("GPR_ROW_DECIMATE", "2", 1);
    setenv("GPR_COL_DECIMATE", "2", 1);
    for (int inlined = 1; inlined >= 0; --inlined) {
        setenv("FUSED_INLINE_TOKENIZE", inlined ? "1" : "0", 1);
        for (int drop = 0; drop <= 1; ++drop) {
            setenv("GPR_DROP_HIGHPASS", drop ? "1" : "0", 1);
            FUSED_ENCODER *ctx = gpr_encode_fused_create(128, 128, 1, 3);
            if (!ctx) return 1;
            uint8_t *encoded = NULL;
            size_t size = 0;
            setenv("FUSED_THREADS", "1", 1);
            if (gpr_encode_fused_frame(ctx, (const uint8_t *)raw, sizeof(raw),
                                       &encoded, &size) || !size) return 2;
            uint8_t *serial = malloc(size);
            if (!serial) return 3;
            memcpy(serial, encoded, size);
            size_t serial_size = size;
            setenv("FUSED_THREADS", "0", 1);
            for (int repeat = 0; repeat < 3; ++repeat) {
                if (gpr_encode_fused_frame(ctx, (const uint8_t *)raw, sizeof(raw),
                                           &encoded, &size) || size != serial_size ||
                    memcmp(serial, encoded, size)) return 4;
            }
            free(serial);
            gpr_encode_fused_destroy(ctx);
            printf("PASS: pool matches serial, inline=%d drop_hp=%d\n", inlined, drop);
        }
    }
    return 0;
}
