/*! @file gpr_video_format.h
 *
 *  @brief Container format for GPR raw video streams.
 *
 *  Wraps a sequence of fused-encoder bitstreams with a clip header
 *  (target bitrate, denoise state, encoding parameters) and per-frame
 *  headers (length + tag), so downstream decoders can:
 *   - know the clip's target rate without measuring
 *   - reject incompatible decoder versions
 *   - seek through frames without parsing bitstream content
 *
 *  Wire format (little-endian, packed):
 *
 *      Clip header (32 bytes, written once at stream start):
 *          [0..3]   magic     = 'GVID' (0x47, 0x56, 0x49, 0x44 on disk)
 *          [4]      version   = 1
 *          [5]      flags     bit 0 = rate_control_enabled
 *                             bit 1 = denoise_enabled
 *                             bits 2-7 = reserved (must be 0)
 *          [6..7]   pixel_format (0..5 live FUSED Bayer set; see fused_encode.h)
 *          [8..9]   quality      (0..11 preset)
 *          [10..11] reserved2 (must be 0)
 *          [12..15] width
 *          [16..19] height
 *          [20..23] fps_x1000   (24.0 fps stored as 24000)
 *          [24..27] target_kbps (0 if rate control disabled; otherwise
 *                                target * 8 * 1024 / 1000 from MB/s)
 *          [28..31] frame_count_hint (0 if unknown at start)
 *
 *      Frame header (16 bytes, written before each frame's bitstream):
 *          [0..3]   magic     = 'FRM\0' (0x46, 0x52, 0x4D, 0x00 on disk)
 *          [4..7]   payload_size (bitstream length in bytes)
 *          [8..15]  frame_tag (caller's tag from submit, e.g. PTS)
 *
 *      Then payload_size bytes of fused-encoder bitstream.
 *      Repeat frame-header + bitstream for each frame.
 *
 *  No clip trailer — readers detect EOF naturally.
 *
 *  ## Versioning and forward compatibility
 *
 *  The version byte (offset 4) governs wire-format compatibility. See the
 *  diagram above for the v1 layout.
 *
 *  Bump policy:
 *   - Stays at 1 only for additive changes that the current strict v1 reader
 *     can reject safely until the reader is updated. Reserved flag bits and
 *     reserved2 are wire-reserved today and must remain zero in emitted files.
 *   - Bumps to 2 (or higher) for **structural** changes a v1 reader cannot
 *     safely ignore. Examples: a new frame-header magic, a new payload
 *     encoding (different wavelet basis / quantizer / bitstream layout), or
 *     a new field that is load-bearing for correct decode.
 *
 *  Reader behavior:
 *   - Readers MUST reject versions newer than they recognize (current code
 *     in `gpr_video_read_clip_header` rejects anything ≠ 1). Never silently
 *     degrade on an unknown version — that hides corrupted output.
 *   - Readers MAY support older versions by branching on the version byte.
 *
 *  v1 clip-header stability matrix:
 *   - [0..3] magic, [4] version              : stable across all v1.x
 *   - [5] flags                              : bits 0..1 defined; bits 2..7
 *                                              rejected by the strict v1 reader
 *   - [6..7] pixel_format, [8..9] quality    : stable
 *   - [10..11] reserved2                     : rejected when nonzero by the
 *                                              strict v1 reader
 *   - [12..27] width, height, fps_x1000,
 *              target_kbps                   : stable
 *   - [28..31] frame_count_hint              : stable (0 = unknown remains
 *                                              the documented sentinel)
 *
 *  What requires a v2 bump: a new frame_header magic, any change to the
 *  payload bitstream encoding (basis, quantizer table, entropy coder), or
 *  any new field whose absence breaks correct decode.
 *
 *  (C) Copyright 2018 GoPro Inc. Licensed under Apache-2.0 or MIT.
 */

#ifndef GPR_VIDEO_FORMAT_H
#define GPR_VIDEO_FORMAT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPR_VIDEO_CLIP_MAGIC    0x44495647u   /* 'GVID' LE */
#define GPR_VIDEO_FRAME_MAGIC   0x004D5246u   /* 'FRM\0' LE */
#define GPR_VIDEO_FORMAT_VERSION 1

#define GPR_VIDEO_FLAG_RATE_CONTROL  0x01
#define GPR_VIDEO_FLAG_DENOISE       0x02
#define GPR_VIDEO_FLAG_MASK          (GPR_VIDEO_FLAG_RATE_CONTROL | GPR_VIDEO_FLAG_DENOISE)

#define GPR_VIDEO_CLIP_HEADER_SIZE  32
#define GPR_VIDEO_FRAME_HEADER_SIZE 16
#define GPR_VIDEO_QUALITY_MAX       11
#define GPR_VIDEO_PIXEL_FORMAT_MAX  5

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t pixel_format;
    uint16_t quality;
    uint16_t reserved2;
    uint32_t width;
    uint32_t height;
    uint32_t fps_x1000;
    uint32_t target_kbps;
    uint32_t frame_count_hint;
} gpr_video_clip_header;

typedef struct {
    uint32_t magic;
    uint32_t payload_size;
    uint64_t frame_tag;
} gpr_video_frame_header;

typedef struct {
    gpr_video_clip_header clip;
    uint32_t frame_count;
    uint64_t first_frame_tag;
    uint64_t last_frame_tag;
    uint64_t payload_bytes;
} gpr_video_stream_info;

/*! @brief Encode a clip header into a 32-byte buffer.
    @return GPR_VIDEO_CLIP_HEADER_SIZE on success, -1 on error. */
int gpr_video_write_clip_header(uint8_t *buf, size_t buf_size,
                                 int width, int height,
                                 int pixel_format, int quality,
                                 double fps, double target_MBps,
                                 int denoise_enabled,
                                 uint32_t frame_count_hint);

/*! @brief Encode a frame header into a 16-byte buffer.
    @return GPR_VIDEO_FRAME_HEADER_SIZE on success, -1 on error. */
int gpr_video_write_frame_header(uint8_t *buf, size_t buf_size,
                                  size_t payload_size,
                                  uint64_t frame_tag);

/*! @brief Parse a clip header. Validates magic, version, and v1 field ranges.
    @return 0 on success, -1 on malformed/incompatible header. */
int gpr_video_read_clip_header(const uint8_t *buf, size_t buf_size,
                                gpr_video_clip_header *out);

/*! @brief Parse a frame header. Validates magic and payload size.
    @return 0 on success, -1 on malformed header. */
int gpr_video_read_frame_header(const uint8_t *buf, size_t buf_size,
                                 gpr_video_frame_header *out);

/*! @brief Validate a complete v1 stream buffer.
    Checks clip header, every frame header, payload bounds, strictly increasing
    frame tags, and frame_count_hint when nonzero. `out` may be NULL.
    @return 0 on success, -1 on malformed/incompatible stream. */
int gpr_video_validate_stream(const uint8_t *buf, size_t buf_size,
                               gpr_video_stream_info *out);

#ifdef __cplusplus
}
#endif

#endif /* GPR_VIDEO_FORMAT_H */
