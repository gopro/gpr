/*! @file fast_gpr.c
 *
 *  @brief Lightweight GPR (TIFF/DNG) parser: extracts VC5 blob without DNG SDK.
 *
 *  GPR files are TIFF containers with VC5-compressed tiles. The DNG SDK takes
 *  ~100ms to parse the container. This lightweight parser extracts just the
 *  VC5 blob in < 1ms by reading only the TIFF header and IFD entries.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* TIFF tag codes */
#define TIFF_TAG_IMAGE_WIDTH     256
#define TIFF_TAG_IMAGE_HEIGHT    257
#define TIFF_TAG_COMPRESSION     259
#define TIFF_TAG_STRIP_OFFSETS    273
#define TIFF_TAG_STRIP_BYTE_COUNTS 279
#define TIFF_TAG_TILE_WIDTH      322
#define TIFF_TAG_TILE_HEIGHT     323
#define TIFF_TAG_TILE_OFFSETS    324
#define TIFF_TAG_TILE_BYTE_COUNTS 325
#define TIFF_TAG_BITS_PER_SAMPLE 258
#define TIFF_TAG_SAMPLES_PER_PIXEL 277

/* VC5 compression code in TIFF */
#define TIFF_COMPRESSION_VC5     9

/* TIFF data types */
#define TIFF_TYPE_SHORT 3
#define TIFF_TYPE_LONG  4

typedef struct {
    const uint8_t *data;
    size_t size;
    int big_endian;  /* 0 = little-endian (Intel), 1 = big-endian (Motorola) */
} TIFF_READER;

static uint16_t tiff_read16(const TIFF_READER *r, size_t offset)
{
    if (offset + 2 > r->size) return 0;
    const uint8_t *p = r->data + offset;
    if (r->big_endian)
        return (uint16_t)((p[0] << 8) | p[1]);
    else
        return (uint16_t)((p[1] << 8) | p[0]);
}

static uint32_t tiff_read32(const TIFF_READER *r, size_t offset)
{
    if (offset + 4 > r->size) return 0;
    const uint8_t *p = r->data + offset;
    if (r->big_endian)
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  | p[3];
    else
        return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[1] << 8)  | p[0];
}

/* Read a TIFF tag value (handles short/long, inline or offset) */
static uint32_t tiff_tag_value(const TIFF_READER *r, size_t entry_offset)
{
    uint16_t type = tiff_read16(r, entry_offset + 2);
    uint32_t count = tiff_read32(r, entry_offset + 4);

    if (count == 1) {
        if (type == TIFF_TYPE_SHORT)
            return tiff_read16(r, entry_offset + 8);
        else
            return tiff_read32(r, entry_offset + 8);
    }
    /* For multi-value tags, return the offset to the values */
    return tiff_read32(r, entry_offset + 8);
}

/* Read array of uint32 values from a TIFF tag */
static int tiff_read_uint32_array(const TIFF_READER *r, size_t entry_offset,
                                   uint32_t *out, int max_count)
{
    uint16_t type = tiff_read16(r, entry_offset + 2);
    uint32_t count = tiff_read32(r, entry_offset + 4);
    if ((int)count > max_count) count = max_count;

    int type_size = (type == TIFF_TYPE_SHORT) ? 2 : 4;
    size_t data_size = count * type_size;

    /* Values inline (<=4 bytes) or at offset */
    size_t data_offset;
    if (data_size <= 4)
        data_offset = entry_offset + 8;
    else
        data_offset = tiff_read32(r, entry_offset + 8);

    for (uint32_t i = 0; i < count; i++) {
        if (type == TIFF_TYPE_SHORT)
            out[i] = tiff_read16(r, data_offset + i * 2);
        else
            out[i] = tiff_read32(r, data_offset + i * 4);
    }

    return (int)count;
}

/*!
    @brief Extract VC5 blob from a GPR (TIFF/DNG) file in memory.

    Parses just the TIFF IFD to find the VC5-compressed tile data.
    Returns a pointer into the input buffer (zero-copy) or NULL on failure.

    @param gpr_data     Pointer to GPR file data in memory
    @param gpr_size     Size of GPR file data
    @param vc5_offset   Output: offset of VC5 blob within gpr_data
    @param vc5_size     Output: size of VC5 blob
    @param image_width  Output: image width (optional, can be NULL)
    @param image_height Output: image height (optional, can be NULL)
    @return 0 on success, -1 on failure
*/
int fast_gpr_extract_vc5(const uint8_t *gpr_data, size_t gpr_size,
                          size_t *vc5_offset, size_t *vc5_size,
                          int *image_width, int *image_height)
{
    if (gpr_size < 8) return -1;

    /* Parse TIFF header */
    TIFF_READER r;
    r.data = gpr_data;
    r.size = gpr_size;

    /* Check byte order */
    if (gpr_data[0] == 0x49 && gpr_data[1] == 0x49)
        r.big_endian = 0;  /* Intel (little-endian) */
    else if (gpr_data[0] == 0x4D && gpr_data[1] == 0x4D)
        r.big_endian = 1;  /* Motorola (big-endian) */
    else
        return -1;

    /* Check TIFF magic */
    if (tiff_read16(&r, 2) != 42) return -1;

    /* Get first IFD offset */
    uint32_t ifd_offset = tiff_read32(&r, 4);

    /* Stack of IFD offsets to visit (handles SubIFDs) */
    uint32_t ifd_stack[16];
    int stack_depth = 0;
    ifd_stack[stack_depth++] = ifd_offset;

    /* Walk IFDs and SubIFDs looking for VC5 compressed tiles */
    while (stack_depth > 0) {
        uint32_t cur_ifd = ifd_stack[--stack_depth];
        if (cur_ifd == 0 || cur_ifd + 2 >= gpr_size) continue;

        uint16_t num_entries = tiff_read16(&r, cur_ifd);
        size_t entries_start = cur_ifd + 2;

        int compression = 0;
        int width = 0, height = 0;
        size_t tile_offsets_entry = 0;
        size_t tile_counts_entry = 0;
        int tile_count = 0;

        /* Scan IFD entries */
        for (int i = 0; i < num_entries; i++) {
            size_t entry = entries_start + (size_t)i * 12;
            if (entry + 12 > gpr_size) break;

            uint16_t tag = tiff_read16(&r, entry);
            uint32_t count = tiff_read32(&r, entry + 4);

            switch (tag) {
                case TIFF_TAG_COMPRESSION:
                    compression = (int)tiff_tag_value(&r, entry);
                    break;
                case TIFF_TAG_IMAGE_WIDTH:
                    width = (int)tiff_tag_value(&r, entry);
                    break;
                case TIFF_TAG_IMAGE_HEIGHT:
                    height = (int)tiff_tag_value(&r, entry);
                    break;
                case TIFF_TAG_TILE_OFFSETS:
                    tile_offsets_entry = entry;
                    tile_count = (int)count;
                    break;
                case TIFF_TAG_TILE_BYTE_COUNTS:
                    tile_counts_entry = entry;
                    break;
                case 330: /* SubIFDs — push onto stack */
                    for (uint32_t s = 0; s < count && stack_depth < 16; s++) {
                        uint32_t sub_off;
                        if (count == 1)
                            sub_off = tiff_tag_value(&r, entry);
                        else {
                            uint32_t arr_off = tiff_read32(&r, entry + 8);
                            sub_off = tiff_read32(&r, arr_off + s * 4);
                        }
                        if (sub_off > 0 && sub_off < gpr_size)
                            ifd_stack[stack_depth++] = sub_off;
                    }
                    break;
                default:
                    break;
            }
        }

        /* Found VC5 IFD? */
        if (compression == TIFF_COMPRESSION_VC5 && tile_offsets_entry && tile_counts_entry) {
            /* Read tile offsets and byte counts */
            uint32_t offsets[32], counts[32];
            int n_tiles = (tile_count > 32) ? 32 : tile_count;

            tiff_read_uint32_array(&r, tile_offsets_entry, offsets, n_tiles);
            tiff_read_uint32_array(&r, tile_counts_entry, counts, n_tiles);

            /* For GPR files, typically there's one tile containing the entire VC5 blob */
            if (n_tiles >= 1 && offsets[0] + counts[0] <= gpr_size) {
                *vc5_offset = offsets[0];
                *vc5_size = counts[0];
                if (image_width) *image_width = width;
                if (image_height) *image_height = height;
                return 0;
            }

            return -1; /* VC5 tile found but invalid */
        }

        /* Push next IFD in chain onto stack */
        size_t next_ifd_pos = entries_start + (size_t)num_entries * 12;
        if (next_ifd_pos + 4 <= gpr_size) {
            uint32_t next_ifd = tiff_read32(&r, next_ifd_pos);
            if (next_ifd > 0 && next_ifd < gpr_size && stack_depth < 16)
                ifd_stack[stack_depth++] = next_ifd;
        }
    }

    return -1; /* No VC5 IFD found */
}
