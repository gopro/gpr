/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_PREVIEW_QUEUE_H
#define GPR_PREVIEW_QUEUE_H
#include "gpr_preview.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gpr_preview_queue gpr_preview_queue;
typedef struct {
    uint64_t index,timestamp_ns,offered_ns,render_ns;
    uint32_t payload_crc32;
    gpr_preview_info image;
    const uint8_t *rgb;
    size_t size_bytes,pitch;
} gpr_preview_frame_view;
/* Runs on the display worker without queue locks. RGB expires on return.
 * Return 0 after accepting the complete image, nonzero to latch sink failure.
 * May call get_stats, but not offer/flush/finish/destroy. A blocked callback cannot
 * be interrupted by cancellation. No callback occurs after finish returns. */
typedef int (*gpr_preview_frame_fn)(void *user,const gpr_preview_frame_view *frame);
typedef struct {
    size_t max_payload_bytes,max_rgb_bytes; /* each 1..128 MiB */
    gpr_preview_frame_fn display;
    void *user;
    uint32_t background; /* 0/1: best-effort lower worker priority, not capture priority */
} gpr_preview_queue_config;
typedef struct {
    uint64_t offered,rendered,discarded,replaced,inflight,inflight_high_water;
    uint64_t payload_peak_bytes,render_ns,callback_ns;
    int error;
    uint32_t background_requested,background_applied;
} gpr_preview_queue_stats;
/* One active frame and one pending frame. Offer copies compressed bytes before
 * success returns, retaining its immutable plan until processing/discard. No
 * capture RAW buffer or writer-owned bytes remain borrowed. A newer offer
 * replaces pending work; it never waits for the renderer/display callback.
 * Payload copies briefly serialize under the queue mutex. Buffers are reserved
 * once at create (two payload buffers and one RGB buffer), within configured
 * limits. Timestamps/indices must strictly increase; gaps are permitted. */
int gpr_preview_queue_create(const gpr_preview_queue_config *config,gpr_preview_queue **out);
int gpr_preview_queue_offer(gpr_preview_queue *queue,const gpr_preview_plan *plan,
    uint64_t index,uint64_t timestamp_ns,const uint8_t *payload,size_t payload_size);
/* Same bounded queue and ownership, but copies reconstructed planes instead
 * of compressed payload. max_payload_bytes bounds the combined three planes;
 * internal plane spacing uses spare reserved capacity when available and falls
 * back to dense storage at tight bounds. Payload stats include that spacing.
 * payload_crc32 binds the original encoded frame. Render timing covers display
 * processing only; producer reconstruction precedes offer. */
int gpr_preview_queue_offer_wavelets(gpr_preview_queue *queue,const gpr_preview_plan *plan,
    uint64_t index,uint64_t timestamp_ns,const gpr_preview_wavelet_planes *planes,uint32_t payload_crc32);
int gpr_preview_queue_get_stats(gpr_preview_queue *queue,gpr_preview_queue_stats *out);
/* Wait for an empty queue without closing it. Concurrent offers can extend the
 * wait and retain normal latest-pending replacement semantics. Cancellation
 * may discard pending work: inspect stats for delivery. Returns the sticky
 * error; an empty cancelled/finished queue can also be flushed. Not a storage
 * durability operation. Callback context must remain alive until return. */
int gpr_preview_queue_flush(gpr_preview_queue *queue);
int gpr_preview_queue_finish(gpr_preview_queue *queue); /* close, drain latest, wait */
void gpr_preview_queue_cancel(gpr_preview_queue *queue); /* close, discard pending */
void gpr_preview_queue_destroy(gpr_preview_queue *queue); /* cancel, wait, free */
/* Offer/stats/cancel/flush/finish may run concurrently. Serialize destruction against
 * every caller and keep callback context alive through finish/destroy. */
#ifdef __cplusplus
}
#endif
#endif
