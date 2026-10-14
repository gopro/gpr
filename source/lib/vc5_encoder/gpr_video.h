/*! @file gpr_video.h
 *
 *  @brief Pipelined video encoder: caller → encoder → writer, three threads.
 *
 *  Wraps the fused encoder for raw video. The point of the pipeline is to
 *  hide write latency on storage with variable throughput (microSD, especially
 *  during internal GC) so the encoder never stalls waiting on I/O.
 *
 *  ## Architecture
 *
 *  Caller thread       Encoder thread          Writer thread
 *  ─────────────       ──────────────          ─────────────
 *      submit() ─→  input ring ─→  encode  ─→  output ring  ─→  writer_fn()
 *
 *  Two SPSC ring buffers; encoder owns one fused-encoder context (which
 *  internally uses 4 worker threads for channel-parallel Pass 1 and
 *  band-parallel Pass 2). One encoder thread is enough because the inner
 *  fused encoder already saturates 4 cores.
 *
 *  ## Backpressure
 *
 *  submit() blocks when the input ring is full — natural backpressure to
 *  the caller. The encoder thread blocks when the output ring is full
 *  (i.e. writer is behind). If you want frame-drop-on-overflow behavior,
 *  poll with the non-blocking variant.
 *
 *  ## Buffer ownership
 *
 *  submit() copies the raw frame into a pre-allocated slot. This is the
 *  safe default but costs one full-frame memcpy per submit (~100 MB at
 *  50 MP). Zero-copy buffer-transfer variant can be added if profile data
 *  shows it matters.
 *
 *  (C) Copyright 2018 GoPro Inc. Licensed under Apache-2.0 or MIT.
 */

#ifndef GPR_VIDEO_H
#define GPR_VIDEO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GPR_VIDEO_ENCODER GPR_VIDEO_ENCODER;

/*!
    @brief Writer callback invoked by the writer thread per encoded frame.

    Called from the writer thread, NOT the caller's thread. Implementation
    should be thread-safe with respect to anything else it touches.

    The vc5 bitstream buffer is owned by the encoder context — do not free
    it, and do not use it after this call returns (it will be reused for
    a later frame).

    @param user_data    Pointer passed at create() time
    @param vc5_bitstream Encoded VC5 bitstream (caller does not own)
    @param size         Size of bitstream in bytes
    @param frame_tag    Tag that was passed to submit() for this frame
    @return    0  → success.
               >0 → recoverable error on this frame; logged in
                    stats.writer_errors and the encoder continues with the
                    next frame.
               <0 → fatal: the encoder marks itself aborted, drops pending
                    frames without invoking writer_fn for them, and unblocks
                    any in-flight gpr_video_encoder_flush() /
                    gpr_video_encoder_destroy(). Use this on unrecoverable
                    I/O errors (disk full, write returned EIO) where the
                    caller wants the pipeline to stop immediately rather
                    than back-pressure indefinitely.
*/
typedef int (*gpr_video_writer_fn)(void *user_data,
                                    const uint8_t *vc5_bitstream,
                                    size_t size,
                                    uint64_t frame_tag);

/*!
    @brief Create a pipelined video encoder.

    @param width         Frame width in pixels
    @param height        Frame height in pixels
    @param pixel_format  0=RGGB12, 1=RGGB14, 2=GBRG12, 3=GBRG14, 4=RGGB16, 5=GBRG16.
                         This live-video wrapper follows the FUSED/.gvid
                         contract; GRBG/BGGR are supported by the standard
                         still/GPR encoder path, not by this live path yet.
    @param quality       VC5 quality preset (0-11, 3=Filmscan-1 default)
    @param ring_depth    Number of in-flight frame slots (2-4 typical).
                         Bigger ring = more memory + more latency hiding.
    @param writer        Callback invoked on each encoded frame
    @param user_data     Opaque pointer passed to writer
    @return              Encoder context, or NULL on allocation failure
*/
GPR_VIDEO_ENCODER *gpr_video_encoder_create(
    int width, int height,
    int pixel_format, int quality,
    int ring_depth,
    gpr_video_writer_fn writer, void *user_data);

/*!
    @brief Same as gpr_video_encoder_create() but with explicit encoder count.

    encoder_count=1 is identical to gpr_video_encoder_create().
    encoder_count=2 enables dual ping-pong mode:
      - Two FUSED_ENCODER contexts run in parallel encoder threads.
      - Frames are dispatched to encoder (frame_tag % 2).
      - The writer thread emits in frame_tag order; out-of-order
        completions are reordered before writer_fn() is called.
      - Memory roughly doubles (2x band buffers + 2x input ring slots).
    Use 2 only on machines with >= 4 cores; on 4-core systems the
    internal 4-thread fused encoder will partially contend but
    complementary memory/compute phases give net throughput win.

    Each encoder maintains independent rate-control state (rc_scale,
    rc_avg_bytes). They converge independently; tracking error on
    average is acceptable for adaptive-bitrate use (perfect lockstep
    isn't required).
*/
GPR_VIDEO_ENCODER *gpr_video_encoder_create_dual(
    int width, int height,
    int pixel_format, int quality,
    int ring_depth, int encoder_count,
    gpr_video_writer_fn writer, void *user_data);

/*!
    @brief Submit a raw Bayer frame for encoding.

    Blocks if the input ring is full (natural backpressure). The frame is
    copied into an internal slot; the caller's buffer can be reused after
    this call returns.

    @param ctx        Encoder context
    @param raw_bayer  Raw Bayer pixel data
    @param raw_size   Size of raw data in bytes (must equal width*height*2)
    @param frame_tag  Caller-supplied tag passed through to writer_fn.
                      MUST be a contiguous sequence 0, 1, 2, ... — the
                      writer thread emits frames in strict tag order
                      starting at 0, so any gap or out-of-order start
                      will deadlock the writer. (If you need wall-clock
                      timestamps in the output, store them yourself in
                      the writer_fn keyed by tag.)
    @return           0 on success, -1 on error (including encoder aborted)
*/
int gpr_video_encoder_submit(GPR_VIDEO_ENCODER *ctx,
                              const uint8_t *raw_bayer, size_t raw_size,
                              uint64_t frame_tag);

/*! @brief Block until all submitted frames have been encoded and written.
           Returns immediately if the encoder has been aborted (writer
           returned <0, or gpr_video_encoder_cancel() was called). */
void gpr_video_encoder_flush(GPR_VIDEO_ENCODER *ctx);

/*! @brief Abort pending work and unblock destroy().

    Marks the encoder aborted. Submitted-but-unwritten frames are dropped
    without invoking writer_fn for them. Subsequent gpr_video_encoder_submit()
    calls return -1. gpr_video_encoder_flush() returns immediately. Threads
    exit on their next loop iteration.

    Use this on app shutdown, user cancellation, or any other path where
    the caller wants the encoder to stop without waiting for storage I/O
    to complete. Safe to call from any thread (including the writer
    callback). Idempotent.
*/
void gpr_video_encoder_cancel(GPR_VIDEO_ENCODER *ctx);

/*! @brief Stop the encoder threads and free all resources.
           If the encoder has not been aborted, implicitly flushes first
           (waiting for pending frames to complete). If aborted (writer
           returned <0, or gpr_video_encoder_cancel() was called), skips
           the flush and tears down immediately. */
void gpr_video_encoder_destroy(GPR_VIDEO_ENCODER *ctx);

/*!
    @brief Optional: enable wavelet-domain denoise on every frame.
    Mirrors gpr_encode_fused_set_denoise(). Must be called before the
    first submit() — changing denoise params mid-stream is not supported.
*/
void gpr_video_encoder_set_denoise(GPR_VIDEO_ENCODER *ctx,
                                    double noise_scale,
                                    double noise_offset,
                                    double strength);

/*!
    @brief Enable adaptive bitrate (target-rate rate control).

    When set, the encoder varies its quantization per frame to track
    the target byte rate, smoothing out the content-dependent swing
    (clean ISO 64 vs noisy ISO 22800 can differ 3-4× in raw output
    size). With rate control, the storage sees a steady-state bitrate
    near @p target_MBps regardless of scene content.

    The controller is a proportional one driven by an EMA of recent
    output sizes vs the target. It converges within ~10 frames after
    a content change.

    Pass target_MBps=0 to disable rate control (fall back to fixed
    quality preset). Call before first submit().

    @param target_MBps  Desired sustained output rate in MB/s
    @param fps          Frame rate the caller intends to submit at;
                        used to convert target_MBps to bytes/frame
*/
void gpr_video_encoder_set_target_bitrate(GPR_VIDEO_ENCODER *ctx,
                                           double target_MBps,
                                           double fps);

/*! @brief Stats snapshot. Cheap; safe to call from any thread. */
typedef struct {
    uint64_t frames_submitted;
    uint64_t frames_encoded;
    uint64_t frames_written;
    uint64_t writer_errors;
    /* Wait events: how often each stage stalled on the next. Useful for
       diagnosing the pipeline's binding constraint. */
    uint64_t submit_waited;    /* caller blocked on full input ring */
    uint64_t encoder_waited;   /* encoder blocked on full output ring */
    uint64_t writer_waited;    /* writer blocked on empty output ring */
} gpr_video_stats;

void gpr_video_encoder_get_stats(const GPR_VIDEO_ENCODER *ctx,
                                  gpr_video_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* GPR_VIDEO_H */
