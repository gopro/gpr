/*! @file gpr_labs_encoder.h
 *
 *  @brief Firmware-facing Labs encoder shim for `.gvid` capture.
 *
 *  This is a thin public C API over gpr_video_encoder plus the v1 `.gvid`
 *  stream headers. It exists so camera/Labs integration code can depend on a
 *  small stable surface while the lower-level encoder keeps its current API.
 */

#ifndef GPR_LABS_ENCODER_H
#define GPR_LABS_ENCODER_H

#include <stddef.h>
#include <stdint.h>

#include "gpr_video.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gpr_labs_encoder gpr_labs_encoder;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint16_t bit_depth;
    uint16_t pixel_format;
    uint16_t quality;
    uint32_t fps_x1000;
    uint32_t target_kbps;
    uint32_t max_inflight_frames;
} gpr_labs_encoder_config;

typedef struct {
    const uint8_t *data;
    size_t size_bytes;
    uint64_t frame_index;
    uint64_t timestamp_ns;
} gpr_labs_frame;

typedef int (*gpr_labs_write_cb)(void *user, const uint8_t *data, size_t size);

gpr_labs_encoder *gpr_labs_encoder_create(const gpr_labs_encoder_config *cfg,
                                           gpr_labs_write_cb write_cb,
                                           void *write_user);
int gpr_labs_encoder_submit(gpr_labs_encoder *enc, const gpr_labs_frame *frame);
/* Drain submitted frames; return -1 on writer error or incomplete delivery.
 * Does not synchronize the application's underlying storage device. */
int gpr_labs_encoder_flush(gpr_labs_encoder *enc);
void gpr_labs_encoder_cancel(gpr_labs_encoder *enc);
void gpr_labs_encoder_destroy(gpr_labs_encoder *enc);

void gpr_labs_encoder_get_stats(const gpr_labs_encoder *enc,
                                gpr_video_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* GPR_LABS_ENCODER_H */
