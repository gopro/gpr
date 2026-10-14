/*! @file test_video_format.c
 *
 *  @brief Roundtrip test for the gpr_video container format (clip header
 *  + per-frame headers). Encodes header → decodes header → compares fields.
 *  Also tests bad inputs (short buffer, wrong magic, future version).
 *
 *  Build:
 *    clang -O2 -o /tmp/test_video_format source/app/test_video_format.c \
 *      build/source/lib/vc5_encoder/libvc5_encoder.a -lpthread -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../lib/vc5_encoder/gpr_video_format.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); return 1; } \
} while (0)

static void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static int write_valid_clip(uint8_t *buf) {
    return gpr_video_write_clip_header(buf, GPR_VIDEO_CLIP_HEADER_SIZE,
                                       1920, 1080, 1, 3, 24.0,
                                       150.0, 1, 12);
}

static size_t append_test_frame(uint8_t *buf, size_t pos, size_t cap,
                                uint64_t tag, uint32_t payload_size) {
    if (pos + GPR_VIDEO_FRAME_HEADER_SIZE + payload_size > cap) return 0;
    int n = gpr_video_write_frame_header(buf + pos, cap - pos, payload_size, tag);
    if (n != GPR_VIDEO_FRAME_HEADER_SIZE) return 0;
    pos += GPR_VIDEO_FRAME_HEADER_SIZE;
    memset(buf + pos, (int)(0x40u + tag), payload_size);
    return pos + payload_size;
}

static size_t build_valid_stream(uint8_t *buf, size_t cap, uint32_t hint) {
    int n = gpr_video_write_clip_header(buf, cap, 640, 360, 4, 3,
                                        24.0, 0.0, 0, hint);
    if (n != GPR_VIDEO_CLIP_HEADER_SIZE) return 0;
    size_t pos = GPR_VIDEO_CLIP_HEADER_SIZE;
    pos = append_test_frame(buf, pos, cap, 0, 3);
    if (pos == 0) return 0;
    pos = append_test_frame(buf, pos, cap, 1, 5);
    return pos;
}

static int test_clip_header_roundtrip(void) {
    uint8_t buf[GPR_VIDEO_CLIP_HEADER_SIZE];
    int n = gpr_video_write_clip_header(buf, sizeof(buf),
                                         /*w*/ 8688, /*h*/ 5800,
                                         /*pf*/ 1, /*q*/ 3,
                                         /*fps*/ 24.0,
                                         /*target_MBps*/ 150.0,
                                         /*denoise*/ 1,
                                         /*frame_count_hint*/ 1440);
    CHECK(n == GPR_VIDEO_CLIP_HEADER_SIZE, "write_clip_header size");

    gpr_video_clip_header h;
    int rc = gpr_video_read_clip_header(buf, sizeof(buf), &h);
    CHECK(rc == 0, "read_clip_header rc");

    CHECK(h.magic == GPR_VIDEO_CLIP_MAGIC, "magic");
    CHECK(h.version == GPR_VIDEO_FORMAT_VERSION, "version");
    CHECK(h.flags == (GPR_VIDEO_FLAG_RATE_CONTROL | GPR_VIDEO_FLAG_DENOISE), "flags");
    CHECK(h.pixel_format == 1, "pixel_format");
    CHECK(h.quality == 3, "quality");
    CHECK(h.width == 8688, "width");
    CHECK(h.height == 5800, "height");
    CHECK(h.fps_x1000 == 24000, "fps_x1000");
    /* 150 MB/s * 8 * 1024 = 1228800 kbps */
    CHECK(h.target_kbps == 1228800u, "target_kbps");
    CHECK(h.frame_count_hint == 1440, "frame_count_hint");
    printf("  PASS  clip header roundtrip (1440-frame, 150 MB/s, denoise on)\n");
    return 0;
}

static int test_clip_header_no_rc(void) {
    uint8_t buf[GPR_VIDEO_CLIP_HEADER_SIZE];
    int n = gpr_video_write_clip_header(buf, sizeof(buf), 1920, 1080, 0, 5,
                                         60.0, 0.0, 0, 0);
    CHECK(n == GPR_VIDEO_CLIP_HEADER_SIZE, "write size");

    gpr_video_clip_header h;
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == 0, "read");
    CHECK((h.flags & GPR_VIDEO_FLAG_RATE_CONTROL) == 0, "no rate control flag");
    CHECK((h.flags & GPR_VIDEO_FLAG_DENOISE) == 0, "no denoise flag");
    CHECK(h.target_kbps == 0, "target_kbps zero");
    CHECK(h.fps_x1000 == 60000, "fps_x1000 60000");
    CHECK(h.frame_count_hint == 0, "unknown frame count");
    printf("  PASS  clip header with rate-control disabled\n");
    return 0;
}

static int test_clip_header_q11(void) {
    uint8_t buf[GPR_VIDEO_CLIP_HEADER_SIZE];
    int n = gpr_video_write_clip_header(buf, sizeof(buf), 1920, 1080, 4, 11,
                                         24.0, 0.0, 0, 24);
    CHECK(n == GPR_VIDEO_CLIP_HEADER_SIZE, "write q11 size");

    gpr_video_clip_header h;
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == 0, "read q11");
    CHECK(h.quality == 11, "q11 preserved");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 1920, 1080, 4, 12,
                                      24.0, 0.0, 0, 24) == -1,
          "q12 rejected");
    printf("  PASS  clip header accepts supported q11 and rejects q12\n");
    return 0;
}

static int test_frame_header_roundtrip(void) {
    uint8_t buf[GPR_VIDEO_FRAME_HEADER_SIZE];
    int n = gpr_video_write_frame_header(buf, sizeof(buf),
                                          /*payload*/ 6345678,
                                          /*tag*/ 0x1234567890abcdefULL);
    CHECK(n == GPR_VIDEO_FRAME_HEADER_SIZE, "write size");

    gpr_video_frame_header h;
    CHECK(gpr_video_read_frame_header(buf, sizeof(buf), &h) == 0, "read");
    CHECK(h.magic == GPR_VIDEO_FRAME_MAGIC, "magic");
    CHECK(h.payload_size == 6345678u, "payload_size");
    CHECK(h.frame_tag == 0x1234567890abcdefULL, "tag");
    printf("  PASS  frame header roundtrip (tag 0x12345...)\n");
    return 0;
}

static int test_bad_inputs(void) {
    uint8_t buf[GPR_VIDEO_CLIP_HEADER_SIZE];
    /* Short write buffer */
    CHECK(gpr_video_write_clip_header(buf, 16, 100, 100, 0, 3, 24.0, 150.0, 1, 0) == -1, "short write rejected");

    /* Pixel-format boundary: live .gvid accepts the current FUSED 0..5 set. */
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, GPR_VIDEO_PIXEL_FORMAT_MAX, 3, 24.0, 150.0, 1, 0) == GPR_VIDEO_CLIP_HEADER_SIZE, "max pixel_format accepted");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, GPR_VIDEO_PIXEL_FORMAT_MAX + 1, 3, 24.0, 150.0, 1, 0) == -1, "bad pixel_format rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, /*q*/ 12, 24.0, 150.0, 1, 0) == -1, "bad quality rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, /*fps*/ 0.0, 150.0, 1, 0) == -1, "bad fps rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, NAN, 150.0, 1, 0) == -1, "nan fps rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, INFINITY, 150.0, 1, 0) == -1, "infinite fps rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 1.0e20, 150.0, 1, 0) == -1, "overflow fps rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, -1.0, 1, 0) == -1, "negative target rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, NAN, 1, 0) == -1, "nan target rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, INFINITY, 1, 0) == -1, "infinite target rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, 1.0e20, 1, 0) == -1, "overflow target rejected");
    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, 0.00001, 1, 0) == -1, "target rounding to zero rejected");

    /* Wrong magic */
    gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, 150.0, 1, 0);
    buf[0] = 'X';
    gpr_video_clip_header h;
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "wrong magic rejected");

    /* Future version */
    gpr_video_write_clip_header(buf, sizeof(buf), 100, 100, 1, 3, 24.0, 150.0, 1, 0);
    buf[4] = 99;  /* version 99 */
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "unknown version rejected");

    /* Short read buffer */
    CHECK(gpr_video_read_clip_header(buf, 16, &h) == -1, "short read rejected");

    printf("  PASS  bad inputs handled cleanly\n");
    return 0;
}

static int test_bad_clip_header_fields(void) {
    uint8_t buf[GPR_VIDEO_CLIP_HEADER_SIZE];
    gpr_video_clip_header h;

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    buf[5] |= 0x80u;
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "unknown flags rejected");

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    put_u16_le(buf + 6, GPR_VIDEO_PIXEL_FORMAT_MAX + 1);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "read bad pixel_format rejected");

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    put_u16_le(buf + 8, 12);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "read bad quality rejected");

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    put_u16_le(buf + 10, 1);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "reserved2 rejected");

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    put_u32_le(buf + 12, 0);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "zero width rejected");

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    put_u32_le(buf + 16, 0);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "zero height rejected");

    CHECK(write_valid_clip(buf) == GPR_VIDEO_CLIP_HEADER_SIZE, "valid base clip");
    put_u32_le(buf + 20, 0);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "zero fps rejected");

    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 1920, 1080, 1, 3,
                                      24.0, 0.0, 0, 12) == GPR_VIDEO_CLIP_HEADER_SIZE,
          "valid no-rate-control clip");
    buf[5] |= GPR_VIDEO_FLAG_RATE_CONTROL;
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "rate-control flag without target rejected");

    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 1920, 1080, 1, 3,
                                      24.0, 0.0, 0, 12) == GPR_VIDEO_CLIP_HEADER_SIZE,
          "valid no-rate-control clip");
    put_u32_le(buf + 24, 12000);
    CHECK(gpr_video_read_clip_header(buf, sizeof(buf), &h) == -1, "target without rate-control flag rejected");

    printf("  PASS  malformed clip fields rejected\n");
    return 0;
}

static int test_bad_frame_header_fields(void) {
    uint8_t buf[GPR_VIDEO_FRAME_HEADER_SIZE];
    gpr_video_frame_header h;

    CHECK(gpr_video_write_frame_header(buf, sizeof(buf), 0, 7) == -1,
          "zero-payload frame write rejected");
    CHECK(gpr_video_read_frame_header(buf, 8, &h) == -1, "short frame read rejected");

    CHECK(gpr_video_write_frame_header(buf, sizeof(buf), 4, 7) == GPR_VIDEO_FRAME_HEADER_SIZE,
          "valid frame");
    buf[0] = 'X';
    CHECK(gpr_video_read_frame_header(buf, sizeof(buf), &h) == -1, "wrong frame magic rejected");

    CHECK(gpr_video_write_frame_header(buf, sizeof(buf), 4, 7) == GPR_VIDEO_FRAME_HEADER_SIZE,
          "valid frame");
    put_u32_le(buf + 4, 0);
    CHECK(gpr_video_read_frame_header(buf, sizeof(buf), &h) == -1, "zero-payload frame read rejected");

    printf("  PASS  malformed frame fields rejected\n");
    return 0;
}

static int test_stream_validation(void) {
    uint8_t buf[256];
    gpr_video_stream_info info;
    size_t len = build_valid_stream(buf, sizeof(buf), 2);
    CHECK(len > 0, "valid stream built");
    CHECK(gpr_video_validate_stream(buf, len, &info) == 0, "valid stream accepted");
    CHECK(info.frame_count == 2, "stream frame count");
    CHECK(info.first_frame_tag == 0, "first frame tag");
    CHECK(info.last_frame_tag == 1, "last frame tag");
    CHECK(info.payload_bytes == 8, "payload byte count");
    CHECK(info.clip.width == 640 && info.clip.height == 360, "stream clip fields");

    printf("  PASS  complete stream validation accepts valid streams\n");
    return 0;
}

static int test_bad_stream_validation(void) {
    uint8_t buf[256];
    size_t len = build_valid_stream(buf, sizeof(buf), 2);
    CHECK(len > 0, "valid stream built");

    CHECK(gpr_video_write_clip_header(buf, sizeof(buf), 640, 360, 4, 3,
                                      24.0, 0.0, 0, 0) == GPR_VIDEO_CLIP_HEADER_SIZE,
          "valid clip-only stream built");
    CHECK(gpr_video_validate_stream(buf, GPR_VIDEO_CLIP_HEADER_SIZE, NULL) == -1,
          "zero-frame stream rejected");

    len = build_valid_stream(buf, sizeof(buf), 2);
    CHECK(len > 0, "valid stream built");
    CHECK(gpr_video_validate_stream(buf, GPR_VIDEO_CLIP_HEADER_SIZE + 8, NULL) == -1,
          "truncated frame header rejected");

    CHECK(gpr_video_validate_stream(buf, len - 1, NULL) == -1, "truncated payload rejected");

    len = build_valid_stream(buf, sizeof(buf), 3);
    CHECK(len > 0, "valid stream built");
    CHECK(gpr_video_validate_stream(buf, len, NULL) == -1, "frame_count_hint mismatch rejected");

    len = build_valid_stream(buf, sizeof(buf), 2);
    CHECK(len > 0, "valid stream built");
    put_u32_le(buf + GPR_VIDEO_CLIP_HEADER_SIZE + GPR_VIDEO_FRAME_HEADER_SIZE + 3 + 8, 0);
    CHECK(gpr_video_validate_stream(buf, len, NULL) == -1, "duplicate frame tag rejected");

    len = build_valid_stream(buf, sizeof(buf), 2);
    CHECK(len > 0, "valid stream built");
    put_u32_le(buf + GPR_VIDEO_CLIP_HEADER_SIZE + 8, 2);
    CHECK(gpr_video_validate_stream(buf, len, NULL) == -1, "out-of-order frame tag rejected");

    len = build_valid_stream(buf, sizeof(buf), 2);
    CHECK(len > 0, "valid stream built");
    put_u32_le(buf + GPR_VIDEO_CLIP_HEADER_SIZE + GPR_VIDEO_FRAME_HEADER_SIZE + 3 + 4, 1000);
    CHECK(gpr_video_validate_stream(buf, len, NULL) == -1, "oversized payload rejected");

    printf("  PASS  malformed streams rejected\n");
    return 0;
}

int main(void) {
    int failed = 0;
    failed += test_clip_header_roundtrip();
    failed += test_clip_header_no_rc();
    failed += test_clip_header_q11();
    failed += test_frame_header_roundtrip();
    failed += test_bad_inputs();
    failed += test_bad_clip_header_fields();
    failed += test_bad_frame_header_fields();
    failed += test_stream_validation();
    failed += test_bad_stream_validation();
    printf("\n%s\n", failed == 0 ? "All tests PASS" : "SOME TESTS FAILED");
    return failed;
}
