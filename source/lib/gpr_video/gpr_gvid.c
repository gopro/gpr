/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_gvid.h"
#include <limits.h>
#include <string.h>
#include <zlib.h>

#if defined(__aarch64__) && !defined(GPR_GVID_SOFTWARE_CRC)
#if defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#define GPR_RUNTIME_ARM_CRC 1
/* The function target enables CRC instructions without changing the baseline
 * executable ISA. Dispatch below checks Linux HWCAP before calling it. */
__attribute__((target("+crc")))
static uint32_t arm_crc(uint32_t crc, const uint8_t *p, size_t size) {
    crc ^= UINT32_MAX;
    while (size >= 8) {
        uint64_t word; memcpy(&word, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        word = __builtin_bswap64(word);
#endif
        __asm__("crc32x %w0, %w0, %1" : "+r"(crc) : "r"(word));
        p += 8; size -= 8;
    }
    while (size--) { uint32_t byte=*p++; __asm__("crc32b %w0, %w0, %w1" : "+r"(crc) : "r"(byte)); }
    return crc ^ UINT32_MAX;
}
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define GPR_STATIC_ARM_CRC 1
static uint32_t arm_crc(uint32_t crc, const uint8_t *p, size_t size) {
    crc ^= UINT32_MAX;
    while (size >= 8) {
        uint64_t word; memcpy(&word, p, 8);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        word = __builtin_bswap64(word);
#endif
        crc=__crc32d(crc,word); p+=8; size-=8;
    }
    while (size--) crc=__crc32b(crc,*p++);
    return crc ^ UINT32_MAX;
}
#endif
#endif

static void put(uint8_t *p, uint64_t n, unsigned bytes) {
    for (unsigned i = 0; i < bytes; ++i) p[i] = (uint8_t)(n >> (i * 8));
}
uint32_t gpr_gvid_crc32(uint32_t previous, const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
#if defined(GPR_RUNTIME_ARM_CRC)
    if (getauxval(AT_HWCAP) & HWCAP_CRC32) return arm_crc(previous,p,size);
#elif defined(GPR_STATIC_ARM_CRC)
    return arm_crc(previous,p,size);
#endif
    uLong crc = previous;
    while (size) {
        uInt n = size > UINT_MAX ? UINT_MAX : (uInt)size;
        crc = crc32(crc, p, n); p += n; size -= n;
    }
    return (uint32_t)crc;
}
static int metadata_valid(const void *data, size_t size) {
    return size <= GPR_GVID_METADATA_LIMIT && (size == 0 || data != NULL);
}
int gpr_gvid_write_header(uint8_t out[64], const gpr_gvid_config *cfg,
                          const void *metadata, size_t size) {
    if (!out || !cfg || !metadata_valid(metadata, size) || !cfg->width || !cfg->height ||
        cfg->width > 8192 || cfg->height > 6144 || cfg->width % 8 || cfg->height % 8 ||
        !cfg->fps_num || !cfg->fps_den || cfg->fps_num > 240000 || cfg->fps_den > 10000 ||
        (uint64_t)cfg->fps_num > 240ULL * cfg->fps_den || cfg->quality > 5 ||
        (cfg->sample_bits != 12 && cfg->sample_bits != 14 && cfg->sample_bits != 16) ||
        (memcmp(cfg->cfa, "RGGB", 4) && memcmp(cfg->cfa, "GBRG", 4) &&
         memcmp(cfg->cfa, "GRBG", 4) && memcmp(cfg->cfa, "BGGR", 4))) return -1;
    memset(out, 0, 64);
    memcpy(out, "GVID", 4); put(out+4, 3, 2); put(out+6, 64, 2);
    memcpy(out+8, "VC5\0", 4); put(out+12, cfg->width, 4); put(out+16, cfg->height, 4);
    put(out+20, cfg->fps_num, 4); put(out+24, cfg->fps_den, 4); put(out+28, cfg->quality, 4);
    memcpy(out+32, cfg->cfa, 4); put(out+36, cfg->sample_bits, 4);
    put(out+40, 48, 4); put(out+44, size, 4); put(out+48, 1, 4);
    put(out+52, gpr_gvid_crc32(0, metadata, size), 4);
    put(out+56, gpr_gvid_crc32(0, out, 56), 4);
    return 0;
}
int gpr_gvid_write_frame_with_crc(uint8_t out[48], uint64_t index, uint64_t timestamp_ns,
                                  size_t payload_size, uint32_t payload_crc32,
                                  const void *metadata, size_t metadata_size) {
    if (!out || !payload_size || !metadata_valid(metadata, metadata_size)) return -1;
    memset(out, 0, 48); memcpy(out, "FRM3", 4); put(out+4, 48, 4);
    put(out+8, index, 8); put(out+16, timestamp_ns, 8); put(out+24, payload_size, 8);
    put(out+32, metadata_size, 4); put(out+36, payload_crc32, 4);
    put(out+40, gpr_gvid_crc32(0, metadata, metadata_size), 4);
    put(out+44, gpr_gvid_crc32(0, out, 44), 4);
    return 0;
}
int gpr_gvid_write_frame(uint8_t out[48], uint64_t index, uint64_t timestamp_ns,
                         const void *payload, size_t payload_size,
                         const void *metadata, size_t metadata_size) {
    if (!payload || !payload_size) return -1;
    return gpr_gvid_write_frame_with_crc(out, index, timestamp_ns, payload_size,
                                         gpr_gvid_crc32(0, payload, payload_size),
                                         metadata, metadata_size);
}
void gpr_gvid_write_end(uint8_t out[48], uint64_t count, uint64_t timestamp_ns,
                        uint64_t payload_bytes, uint32_t stream_crc32) {
    memset(out, 0, 48); memcpy(out, "END3", 4); put(out+4, 48, 4);
    put(out+8, count, 8); put(out+16, timestamp_ns, 8); put(out+24, payload_bytes, 8);
    put(out+32, stream_crc32, 4); put(out+36, gpr_gvid_crc32(0, out, 36), 4);
}
