/*! @file gpr_labs_extension_api.h
 *
 *  @brief Draft Labs raw media extension ABI.
 *
 *  This header is a proposal surface for Labs firmware review. It is not wired
 *  into the SDK build by this change.
 *
 *  (C) Copyright 2026.
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 */

#ifndef GPR_LABS_EXTENSION_API_H
#define GPR_LABS_EXTENSION_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GPR_LABS_EXTENSION_ABI_VERSION 1
#define GPR_LABS_EXTENSION_ID_SIZE 64
#define GPR_LABS_EXTENSION_NAME_SIZE 64
#define GPR_LABS_EXTENSION_MODE_SIZE 64
#define GPR_LABS_EXTENSION_ERROR_SIZE 128
#define GPR_LABS_BIT_DEPTH_14_MASK (1u << 14)
#define GPR_LABS_BIT_DEPTH_16_MASK (1u << 16)

typedef enum
{
    GPR_LABS_STATUS_OK = 0,
    GPR_LABS_STATUS_UNSUPPORTED_ABI = 1,
    GPR_LABS_STATUS_UNSUPPORTED_MODE = 2,
    GPR_LABS_STATUS_INVALID_ARGUMENT = 3,
    GPR_LABS_STATUS_RESOURCE_LIMIT = 4,
    GPR_LABS_STATUS_IO_ERROR = 5,
    GPR_LABS_STATUS_INTERNAL_ERROR = 6,
} gpr_labs_status;

typedef enum
{
    GPR_LABS_PIXEL_FORMAT_RGGB14 = 1,
    GPR_LABS_PIXEL_FORMAT_GBRG14 = 2,
    GPR_LABS_PIXEL_FORMAT_BGGR14 = 3,
    GPR_LABS_PIXEL_FORMAT_GRBG14 = 4,
    GPR_LABS_PIXEL_FORMAT_RGGB16 = 5,
    GPR_LABS_PIXEL_FORMAT_GBRG16 = 6,
    GPR_LABS_PIXEL_FORMAT_BGGR16 = 7,
    GPR_LABS_PIXEL_FORMAT_GRBG16 = 8,
} gpr_labs_pixel_format;

typedef enum
{
    GPR_LABS_SOURCE_SENSOR_DMA_CAPTURE = 1,
    GPR_LABS_SOURCE_CAMERA_RING_BUFFER = 2,
    GPR_LABS_SOURCE_FILE_STANDIN = 3,
} gpr_labs_raw_source_kind;

typedef struct
{
    uint32_t abi_version;
    char extension_id[GPR_LABS_EXTENSION_ID_SIZE];
    char extension_name[GPR_LABS_EXTENSION_NAME_SIZE];
    uint32_t mode_count;
    uint32_t requires_raw_bayer_frame_source;
    uint32_t requires_sd_writer;
    uint32_t requires_preview_surface;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t supported_bit_depth_mask;
    uint32_t max_fps_num;
    uint32_t max_fps_den;
    uint32_t max_rss_mb;
    uint32_t max_frame_time_us;
    uint32_t max_write_mbps;
} gpr_labs_extension_caps;

typedef struct
{
    char mode_id[GPR_LABS_EXTENSION_MODE_SIZE];
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
    gpr_labs_pixel_format pixel_format;
    uint32_t bit_depth;
    gpr_labs_raw_source_kind source_kind;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t max_frame_time_us;
    uint32_t max_write_mbps;
} gpr_labs_capture_config;

typedef struct
{
    const void* data;
    uint32_t size_bytes;
    uint32_t stride_bytes;
    uint64_t timestamp_us;
    uint32_t frame_number;
} gpr_labs_raw_frame;

typedef struct
{
    void* data;
    uint32_t capacity_bytes;
    uint32_t bytes_written;
} gpr_labs_output_buffer;

typedef struct
{
    void* rgba_or_rgb_data;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t format;
} gpr_labs_preview_surface;

typedef struct
{
    uint32_t frames_written;
    uint32_t frames_dropped;
    uint32_t width;
    uint32_t height;
    uint32_t preview_width;
    uint32_t preview_height;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t median_frame_time_us;
    uint32_t wall_frame_time_us;
    uint32_t write_mbps;
    uint32_t max_rss_mb;
    uint32_t output_valid;
    uint32_t preview_presented;
    char last_error[GPR_LABS_EXTENSION_ERROR_SIZE];
} gpr_labs_capture_receipt;

typedef struct gpr_labs_extension_session gpr_labs_extension_session;

typedef gpr_labs_status (*gpr_labs_query_caps_fn)(
    gpr_labs_extension_caps* caps);

typedef gpr_labs_status (*gpr_labs_open_capture_fn)(
    const gpr_labs_capture_config* config,
    gpr_labs_extension_session** session);

typedef gpr_labs_status (*gpr_labs_encode_frame_fn)(
    gpr_labs_extension_session* session,
    const gpr_labs_raw_frame* frame,
    gpr_labs_output_buffer* output);

typedef gpr_labs_status (*gpr_labs_render_preview_fn)(
    gpr_labs_extension_session* session,
    const gpr_labs_raw_frame* frame,
    gpr_labs_preview_surface* preview);

typedef gpr_labs_status (*gpr_labs_close_capture_fn)(
    gpr_labs_extension_session* session,
    gpr_labs_capture_receipt* receipt);

typedef struct
{
    uint32_t abi_version;
    gpr_labs_query_caps_fn query_caps;
    gpr_labs_open_capture_fn open_capture;
    gpr_labs_encode_frame_fn encode_frame;
    gpr_labs_render_preview_fn render_preview;
    gpr_labs_close_capture_fn close_capture;
} gpr_labs_extension_api;

#ifdef __cplusplus
}
#endif

#endif /* GPR_LABS_EXTENSION_API_H */
