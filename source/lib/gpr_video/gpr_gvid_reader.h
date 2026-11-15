/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_GVID_READER_H
#define GPR_GVID_READER_H
#include "gpr_gvid.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct gpr_gvid_reader gpr_gvid_reader;
enum { GPR_GVID_FRAME=0, GPR_GVID_DONE=1, GPR_GVID_INVALID=-1,
       GPR_GVID_TRUNCATED=-2, GPR_GVID_IO=-3, GPR_GVID_NOMEM=-4, GPR_GVID_LIMIT=-5 };
/* Partial reads are permitted; 0 means EOF, negative means I/O failure.
 * Serialize reader calls. The callback must not reenter the reader. */
typedef ptrdiff_t (*gpr_gvid_read_fn)(void *user,uint8_t *destination,size_t bytes);
typedef struct {
    gpr_gvid_config video;
    uint32_t version;
    const uint8_t *metadata;
    size_t metadata_size;
} gpr_gvid_stream_info;
typedef struct {
    uint64_t index,timestamp_ns;
    const uint8_t *payload,*metadata;
    size_t payload_size,metadata_size;
} gpr_gvid_frame_view;
/* max_payload=0 selects a 128 MiB operational limit, also bounded by geometry.
 * Metadata remains opaque here; schema/JSON validation belongs to the caller.
 * Frame bytes expire on the next next() or close(). Stream metadata is stable
 * until close(). V3 is complete only after next() returns DONE and verified=1.
 * V2 has no end marker; DONE validates physical EOF but verified remains 0.
 * An error is sticky; a previously delivered frame is not proof of a complete
 * recording. No automatic recovery or silent EOF acceptance is performed. */
int gpr_gvid_reader_create(gpr_gvid_read_fn read,void *user,size_t max_payload,gpr_gvid_reader **out);
int gpr_gvid_reader_info(gpr_gvid_reader *reader,gpr_gvid_stream_info *out);
int gpr_gvid_reader_next(gpr_gvid_reader *reader,gpr_gvid_frame_view *out);
int gpr_gvid_reader_end_verified(gpr_gvid_reader *reader);
void gpr_gvid_reader_close(gpr_gvid_reader *reader);
#ifdef __cplusplus
}
#endif
#endif
