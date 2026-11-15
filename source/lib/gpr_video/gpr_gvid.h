/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_GVID_H
#define GPR_GVID_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Stock-payload GVID v3. Existing fused v1 and experimental v2 are distinct.
 * Integers are little-endian; reserved bytes must be zero. Metadata is opaque
 * application-owned UTF-8 JSON, at most 64 KiB per clip/frame. The caller is
 * responsible for its schema; the container does not interpret camera data.
 *
 * Clip 64: GVID, u16 version=3, u16 bytes=64, VC5\0, u32 w,h,fps_num,
 * fps_den,quality, CFA[4] (RGGB/GBRG/GRBG/BGGR),
 * u32 sample_bits (12/14/16),frame_header_bytes=48,
 * metadata_bytes,flags=1,metadata_crc32,header_crc32(first 56 bytes),reserved;
 * then metadata.
 * Frame 48: FRM3,u32 bytes=48,u64 index,timestamp_ns,payload_bytes,
 * u32 metadata_bytes,payload_crc32,metadata_crc32,header_crc32(first 44 bytes);
 * then metadata,payload.
 * End 48: END3,u32 bytes=48,u64 count,last_timestamp,payload_bytes,
 * u32 stream_crc32,end_crc32(first 36 bytes),reserved[8].
 * Stream CRC covers all clip/frame header+metadata+payload bytes, not END3.
 */
#define GPR_GVID_HEADER_BYTES 64
#define GPR_GVID_RECORD_BYTES 48
#define GPR_GVID_METADATA_LIMIT 65536

typedef struct {
    uint32_t width, height, fps_num, fps_den, quality;
    uint32_t sample_bits;
    char cfa[4];
} gpr_gvid_config;

uint32_t gpr_gvid_crc32(uint32_t previous, const void *data, size_t size);
int gpr_gvid_write_header(uint8_t out[64], const gpr_gvid_config *cfg,
                          const void *metadata, size_t metadata_size);
int gpr_gvid_write_frame(uint8_t out[48], uint64_t index, uint64_t timestamp_ns,
                         const void *payload, size_t payload_size,
                         const void *metadata, size_t metadata_size);
int gpr_gvid_write_frame_with_crc(uint8_t out[48], uint64_t index, uint64_t timestamp_ns,
                                  size_t payload_size, uint32_t payload_crc32,
                                  const void *metadata, size_t metadata_size);
void gpr_gvid_write_end(uint8_t out[48], uint64_t count, uint64_t timestamp_ns,
                        uint64_t payload_bytes, uint32_t stream_crc32);
#ifdef __cplusplus
}
#endif
#endif
