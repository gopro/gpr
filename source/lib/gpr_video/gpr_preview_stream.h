/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_PREVIEW_STREAM_H
#define GPR_PREVIEW_STREAM_H
#include "gpr_preview.h"
#include "vc5_preview.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gpr_preview_stream gpr_preview_stream;
/* Preallocate a quarter-sensor GS/RG/BG streaming inverse. The combined sample
 * buffers (including final planes) must fit within max_bytes, 1..128 MiB.
 * No per-row allocation. Geometry follows the stock encoder's even >=48
 * dimensions and signed-int image-span limit. No RAW/RGB metadata is changed. */
int gpr_preview_stream_create(uint32_t sensor_width,uint32_t sensor_height,
    size_t max_bytes,gpr_preview_stream **out);
/* Different channels may push concurrently; serialize pushes within a channel.
 * Rows must have stock forward-pipeline order and compatible quant/prescale.
 * Input pointers expire on return. Invalid rows latch a frame error; encoding
 * may continue, but finish will not expose a partial preview. */
int gpr_preview_stream_push(gpr_preview_stream *stream,const vc5_preview_row *row);
/* Call only after all producer callbacks have returned. Completed planes are
 * borrowed, immutable until reset/destroy, and may be rendered/offered through
 * the existing calibrated wavelet API. Failure clears out and leaves no
 * borrowed partial planes. This is not a recording/durability operation. */
int gpr_preview_stream_finish(gpr_preview_stream *stream,gpr_preview_wavelet_planes *out);
/* Reset also discards partial/error frames. Reset/destroy/finish must not run
 * concurrently with pushes or borrowed-plane readers. No callback/thread is
 * owned by this object. Keep it alive until the encoder has returned. */
int gpr_preview_stream_reset(gpr_preview_stream *stream);
void gpr_preview_stream_destroy(gpr_preview_stream *stream);
/* Combined allocated sample-buffer span including final planes and cubic table;
 * excludes object/vector bookkeeping and allocator overhead. Immutable. */
size_t gpr_preview_stream_buffer_bytes(const gpr_preview_stream *stream);
#ifdef __cplusplus
}
#endif
#endif
