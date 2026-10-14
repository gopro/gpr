/* Small validation fixture writer; no capture pipeline or DNG SDK. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fused_encode.h"
#include "gpr_video_format.h"

static int parse_uint(const char *text, int max, int *value)
{
    unsigned n = 0;
    if (!*text) return -1;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return -1;
        n = n * 10 + (unsigned)(*text - '0');
        if (n > (unsigned)max) return -1;
    }
    *value = (int)n;
    return 0;
}

int main(int argc, char **argv)
{
    int width, height, pf;
    int status = 1;
    FILE *input = NULL, *output = NULL;
    uint8_t *raw = NULL, *payload = NULL;
    size_t payload_size = 0;
    uint8_t clip[GPR_VIDEO_CLIP_HEADER_SIZE];
    uint8_t frame[GPR_VIDEO_FRAME_HEADER_SIZE];

    if (argc != 6 || parse_uint(argv[2], 1024, &width) ||
        parse_uint(argv[3], 1024, &height) || parse_uint(argv[4], 4, &pf) ||
        width < 128 || height < 128 || width % 128 || height % 128 ||
        (pf != 0 && pf != 1 && pf != 4)) {
        fprintf(stderr, "usage: test_preview_fixture raw width height pf output\n"
                        "dimensions: multiples of 128, 128..1024; pf: 0, 1, 4\n");
        return 1;
    }

    const size_t raw_size = (size_t)width * (size_t)height * 2;
    raw = (uint8_t *)malloc(raw_size);
    input = fopen(argv[1], "rb");
    if (!raw || !input) {
        fprintf(stderr, "cannot allocate or open raw input\n");
        goto cleanup;
    }
    if (fread(raw, 1, raw_size, input) != raw_size ||
        fgetc(input) != EOF || ferror(input)) {
        fprintf(stderr, "raw input must contain exactly width * height * 2 bytes\n");
        goto cleanup;
    }

    if (setenv("GPR_INCLUDE_LL", "1", 1) ||
        setenv("FUSED_MULTI_LEVEL", "0", 1) ||
        setenv("GPR_COL_DECIMATE", "1", 1) ||
        setenv("GPR_ROW_DECIMATE", "1", 1) ||
        setenv("FUSED_THREADS", "1", 1)) {
        fprintf(stderr, "cannot set fixture encoder options\n");
        goto cleanup;
    }
    if (gpr_encode_fused(raw, raw_size, width, height, pf, 3,
                         &payload, &payload_size) != 0 || !payload || !payload_size) {
        fprintf(stderr, "fixture encode failed\n");
        goto cleanup;
    }
    if (gpr_video_write_clip_header(clip, sizeof(clip), width, height, pf, 3,
                                    24.0, 0.0, 0, 2) != (int)sizeof(clip)) {
        fprintf(stderr, "fixture clip header failed\n");
        goto cleanup;
    }
    output = fopen(argv[5], "wb");
    if (!output) {
        fprintf(stderr, "cannot open fixture output\n");
        goto cleanup;
    }
    if (fwrite(clip, 1, sizeof(clip), output) != sizeof(clip)) goto write_error;
    for (unsigned tag = 0; tag < 2; ++tag) {
        if (gpr_video_write_frame_header(frame, sizeof(frame), payload_size, tag)
                != (int)sizeof(frame) ||
            fwrite(frame, 1, sizeof(frame), output) != sizeof(frame) ||
            fwrite(payload, 1, payload_size, output) != payload_size)
            goto write_error;
    }
    if (fclose(output) != 0) {
        output = NULL;
        goto write_error;
    }
    output = NULL;
    status = 0;
    goto cleanup;

write_error:
    fprintf(stderr, "cannot write fixture output\n");
cleanup:
    if (input) fclose(input);
    if (output) fclose(output);
    free(payload);
    free(raw);
    return status;
}
