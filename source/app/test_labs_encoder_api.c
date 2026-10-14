#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/vc5_encoder/gpr_labs_encoder.h"
#include "../lib/vc5_encoder/gpr_video_format.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); return 1; } \
} while (0)

enum {
    W = 128,
    H = 96,
    PF = 4,
    QUALITY = 3,
    FPS_X1000 = 20000
};

typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} mem_writer;

static int append_bytes(void *user, const uint8_t *data, size_t size)
{
    mem_writer *w = (mem_writer *)user;
    if (!w || !data) return -1;
    if (size > ((size_t)-1) - w->size) return -1;
    size_t need = w->size + size;
    if (need > w->cap) {
        size_t new_cap = w->cap ? w->cap * 2u : 4096u;
        while (new_cap < need) {
            if (new_cap > ((size_t)-1) / 2u) return -1;
            new_cap *= 2u;
        }
        uint8_t *p = (uint8_t *)realloc(w->data, new_cap);
        if (!p) return -1;
        w->data = p;
        w->cap = new_cap;
    }
    memcpy(w->data + w->size, data, size);
    w->size += size;
    return 0;
}

static void fill_frame(uint8_t *buf, size_t stride, int seed)
{
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(void *)(buf + (size_t)y * stride);
        for (int x = 0; x < W; x++) {
            row[x] = (uint16_t)((seed * 97 + y * 31 + x * 17 + (x * y)) & 0x3fff);
        }
    }
}

static int encode_stream_with_stride(size_t stride, uint32_t target_kbps)
{
    mem_writer w = {0};
    gpr_labs_encoder_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.width = W;
    cfg.height = H;
    cfg.stride_bytes = (uint32_t)stride;
    cfg.bit_depth = 16;
    cfg.pixel_format = PF;
    cfg.quality = QUALITY;
    cfg.fps_x1000 = FPS_X1000;
    cfg.target_kbps = target_kbps;
    cfg.max_inflight_frames = 2;

    gpr_labs_encoder *enc = gpr_labs_encoder_create(&cfg, append_bytes, &w);
    CHECK(enc != NULL, "create encoder");

    uint8_t *frame = (uint8_t *)malloc(stride * H);
    CHECK(frame != NULL, "allocate frame");
    for (uint64_t i = 0; i < 3; i++) {
        fill_frame(frame, stride, (int)i);
        gpr_labs_frame f;
        memset(&f, 0, sizeof f);
        f.data = frame;
        f.size_bytes = stride * H;
        f.frame_index = i;
        f.timestamp_ns = i * 50000000ull;
        CHECK(gpr_labs_encoder_submit(enc, &f) == 0, "submit sequential frame");
    }

    CHECK(gpr_labs_encoder_flush(enc) == 0, "flush encoder");
    gpr_video_stats stats;
    memset(&stats, 0, sizeof stats);
    gpr_labs_encoder_get_stats(enc, &stats);
    gpr_labs_encoder_destroy(enc);
    free(frame);

    CHECK(stats.frames_submitted == 3, "submitted stats");
    CHECK(stats.frames_written == 3, "written stats");

    gpr_video_stream_info info;
    memset(&info, 0, sizeof info);
    CHECK(gpr_video_validate_stream(w.data, w.size, &info) == 0, "validate stream");
    CHECK(info.clip.width == W, "clip width");
    CHECK(info.clip.height == H, "clip height");
    CHECK(info.clip.pixel_format == PF, "clip pixel format");
    CHECK(info.clip.quality == QUALITY, "clip quality");
    CHECK(info.clip.fps_x1000 == FPS_X1000, "clip fps");
    CHECK(info.clip.target_kbps == target_kbps, "clip target kbps");
    CHECK(info.frame_count == 3, "frame count");
    CHECK(info.first_frame_tag == 0, "first tag");
    CHECK(info.last_frame_tag == 2, "last tag");
    free(w.data);
    return 0;
}

static int reject_out_of_order(void)
{
    mem_writer w = {0};
    gpr_labs_encoder_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.width = W;
    cfg.height = H;
    cfg.stride_bytes = W * 2u;
    cfg.bit_depth = 16;
    cfg.pixel_format = PF;
    cfg.quality = QUALITY;
    cfg.fps_x1000 = FPS_X1000;
    cfg.max_inflight_frames = 2;

    gpr_labs_encoder *enc = gpr_labs_encoder_create(&cfg, append_bytes, &w);
    CHECK(enc != NULL, "create out-of-order encoder");
    uint8_t *frame = (uint8_t *)malloc((size_t)W * H * 2u);
    CHECK(frame != NULL, "allocate out-of-order frame");
    fill_frame(frame, W * 2u, 0);

    gpr_labs_frame f;
    memset(&f, 0, sizeof f);
    f.data = frame;
    f.size_bytes = (size_t)W * H * 2u;
    f.frame_index = 1;
    CHECK(gpr_labs_encoder_submit(enc, &f) != 0, "reject nonzero first tag");

    gpr_labs_encoder_destroy(enc);
    free(frame);
    free(w.data);
    return 0;
}

static int failing_writer(void *user, const uint8_t *data, size_t size)
{
    (void)user; (void)data; (void)size;
    return -1;
}

static int report_writer_failure(void)
{
    gpr_labs_encoder_config cfg = {0};
    cfg.width = W;
    cfg.height = H;
    cfg.stride_bytes = W * 2u;
    cfg.bit_depth = 16;
    cfg.pixel_format = PF;
    cfg.quality = QUALITY;
    cfg.fps_x1000 = FPS_X1000;
    cfg.max_inflight_frames = 2;
    gpr_labs_encoder *enc = gpr_labs_encoder_create(&cfg, failing_writer, NULL);
    CHECK(enc != NULL, "create failing-writer encoder");
    uint8_t *data = malloc((size_t)W * H * 2u);
    CHECK(data != NULL, "allocate failing-writer frame");
    fill_frame(data, W * 2u, 0);
    gpr_labs_frame frame = {0};
    frame.data = data;
    frame.size_bytes = (size_t)W * H * 2u;
    CHECK(gpr_labs_encoder_submit(enc, &frame) == 0, "submit before writer failure");
    int rc = gpr_labs_encoder_flush(enc);
    gpr_labs_encoder_destroy(enc);
    free(data);
    CHECK(rc != 0, "flush must report writer failure");
    return 0;
}

int main(void)
{
    int fails = 0;
    fails += encode_stream_with_stride((size_t)W * 2u, 0);
    fails += encode_stream_with_stride((size_t)W * 2u + 32u, 800000u);
    fails += reject_out_of_order();
    fails += report_writer_failure();
    if (fails) {
        fprintf(stderr, "Labs encoder API tests: %d failure(s)\n", fails);
        return 1;
    }
    fprintf(stderr, "Labs encoder API tests: PASS\n");
    return 0;
}
