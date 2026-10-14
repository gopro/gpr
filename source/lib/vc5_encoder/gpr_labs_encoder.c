#include "gpr_labs_encoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "gpr_video_format.h"

struct gpr_labs_encoder {
    gpr_labs_encoder_config cfg;
    GPR_VIDEO_ENCODER *video;
    gpr_labs_write_cb write_cb;
    void *write_user;
    size_t row_bytes;
    size_t raw_bytes;
    uint8_t *packed_frame;
    uint64_t next_frame_index;
    int clip_header_written;
};

static int mul_size_checked(size_t a, size_t b, size_t *out)
{
    if (!out) return -1;
    if (a != 0 && b > ((size_t)-1) / a) return -1;
    *out = a * b;
    return 0;
}

static double labs_target_MBps(uint32_t target_kbps)
{
    if (target_kbps == 0) return 0.0;
    return (double)target_kbps / (8.0 * 1024.0);
}

static int labs_env_int(const char *name, int fallback)
{
    const char *s = getenv(name);
    char *end = NULL;
    long v;
    if (!s || !*s) return fallback;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return fallback;
    if (v < 1) return fallback;
    if (v > 2) v = 2;
    return (int)v;
}

static int labs_write_all(gpr_labs_encoder *enc,
                          const uint8_t *data, size_t size)
{
    if (!enc || !enc->write_cb || !data || size == 0) return -1;
    return enc->write_cb(enc->write_user, data, size) == 0 ? 0 : -1;
}

static int labs_video_writer(void *user_data,
                             const uint8_t *vc5_bitstream,
                             size_t size,
                             uint64_t frame_tag)
{
    gpr_labs_encoder *enc = (gpr_labs_encoder *)user_data;
    if (!enc || !vc5_bitstream || size == 0) return -1;

    if (!enc->clip_header_written) {
        uint8_t hdr[GPR_VIDEO_CLIP_HEADER_SIZE];
        double fps = (double)enc->cfg.fps_x1000 / 1000.0;
        double target_MBps = labs_target_MBps(enc->cfg.target_kbps);
        int n = gpr_video_write_clip_header(hdr, sizeof hdr,
                                            (int)enc->cfg.width,
                                            (int)enc->cfg.height,
                                            (int)enc->cfg.pixel_format,
                                            (int)enc->cfg.quality,
                                            fps, target_MBps,
                                            0, 0);
        if (n != GPR_VIDEO_CLIP_HEADER_SIZE) return -1;
        if (labs_write_all(enc, hdr, sizeof hdr) != 0) return -1;
        enc->clip_header_written = 1;
    }

    uint8_t frame_hdr[GPR_VIDEO_FRAME_HEADER_SIZE];
    int n = gpr_video_write_frame_header(frame_hdr, sizeof frame_hdr,
                                         size, frame_tag);
    if (n != GPR_VIDEO_FRAME_HEADER_SIZE) return -1;
    if (labs_write_all(enc, frame_hdr, sizeof frame_hdr) != 0) return -1;
    if (labs_write_all(enc, vc5_bitstream, size) != 0) return -1;
    return 0;
}

static int validate_config(const gpr_labs_encoder_config *cfg,
                           size_t *row_bytes,
                           size_t *raw_bytes)
{
    if (!cfg || !row_bytes || !raw_bytes) return -1;
    if (cfg->width == 0 || cfg->height == 0) return -1;
    if (cfg->width > (uint32_t)INT_MAX || cfg->height > (uint32_t)INT_MAX) {
        return -1;
    }
    if (cfg->pixel_format > GPR_VIDEO_PIXEL_FORMAT_MAX) return -1;
    if (cfg->quality > GPR_VIDEO_QUALITY_MAX) return -1;
    if (cfg->fps_x1000 == 0) return -1;
    if (cfg->bit_depth == 0 || cfg->bit_depth > 16) return -1;

    if (mul_size_checked((size_t)cfg->width, 2u, row_bytes) != 0) return -1;
    if (cfg->stride_bytes < *row_bytes) return -1;
    if (mul_size_checked(*row_bytes, (size_t)cfg->height, raw_bytes) != 0) {
        return -1;
    }
    return 0;
}

gpr_labs_encoder *gpr_labs_encoder_create(const gpr_labs_encoder_config *cfg,
                                           gpr_labs_write_cb write_cb,
                                           void *write_user)
{
    size_t row_bytes = 0;
    size_t raw_bytes = 0;
    if (validate_config(cfg, &row_bytes, &raw_bytes) != 0 || !write_cb) {
        return NULL;
    }

    gpr_labs_encoder *enc = (gpr_labs_encoder *)calloc(1, sizeof(*enc));
    if (!enc) return NULL;
    enc->cfg = *cfg;
    enc->write_cb = write_cb;
    enc->write_user = write_user;
    enc->row_bytes = row_bytes;
    enc->raw_bytes = raw_bytes;

    int ring_depth = (int)cfg->max_inflight_frames;
    if (ring_depth <= 0) ring_depth = 3;

    int encoder_count = labs_env_int("GPR_LABS_ENCODER_COUNT", 1);
    enc->video = gpr_video_encoder_create_dual((int)cfg->width,
                                               (int)cfg->height,
                                               (int)cfg->pixel_format,
                                               (int)cfg->quality,
                                               ring_depth,
                                               encoder_count,
                                               labs_video_writer,
                                               enc);
    if (!enc->video) {
        free(enc);
        return NULL;
    }

    if (cfg->target_kbps > 0) {
        gpr_video_encoder_set_target_bitrate(
            enc->video,
            labs_target_MBps(cfg->target_kbps),
            (double)cfg->fps_x1000 / 1000.0);
    }

    if ((size_t)cfg->stride_bytes != row_bytes) {
        enc->packed_frame = (uint8_t *)malloc(raw_bytes);
        if (!enc->packed_frame) {
            gpr_video_encoder_destroy(enc->video);
            free(enc);
            return NULL;
        }
    }

    return enc;
}

int gpr_labs_encoder_submit(gpr_labs_encoder *enc, const gpr_labs_frame *frame)
{
    if (!enc || !enc->video || !frame || !frame->data) return -1;
    if (frame->frame_index != enc->next_frame_index) return -1;

    size_t min_size = 0;
    if (mul_size_checked((size_t)enc->cfg.stride_bytes,
                         (size_t)enc->cfg.height,
                         &min_size) != 0) {
        return -1;
    }
    if (frame->size_bytes < min_size) return -1;

    const uint8_t *submit_data = frame->data;
    if (enc->packed_frame) {
        for (uint32_t y = 0; y < enc->cfg.height; y++) {
            memcpy(enc->packed_frame + (size_t)y * enc->row_bytes,
                   frame->data + (size_t)y * (size_t)enc->cfg.stride_bytes,
                   enc->row_bytes);
        }
        submit_data = enc->packed_frame;
    }

    (void)frame->timestamp_ns;
    int rc = gpr_video_encoder_submit(enc->video, submit_data, enc->raw_bytes,
                                      frame->frame_index);
    if (rc != 0) return -1;
    enc->next_frame_index++;
    return 0;
}

int gpr_labs_encoder_flush(gpr_labs_encoder *enc)
{
    if (!enc || !enc->video) return -1;
    gpr_video_encoder_flush(enc->video);
    gpr_video_stats stats;
    memset(&stats, 0, sizeof stats);
    gpr_video_encoder_get_stats(enc->video, &stats);
    return (stats.writer_errors == 0 &&
            stats.frames_written == stats.frames_submitted) ? 0 : -1;
}

void gpr_labs_encoder_cancel(gpr_labs_encoder *enc)
{
    if (!enc || !enc->video) return;
    gpr_video_encoder_cancel(enc->video);
}

void gpr_labs_encoder_destroy(gpr_labs_encoder *enc)
{
    if (!enc) return;
    if (enc->video) gpr_video_encoder_destroy(enc->video);
    free(enc->packed_frame);
    free(enc);
}

void gpr_labs_encoder_get_stats(const gpr_labs_encoder *enc,
                                gpr_video_stats *out)
{
    if (!enc || !enc->video || !out) return;
    gpr_video_encoder_get_stats(enc->video, out);
}
