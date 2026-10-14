/* gvid_mov.h — GVIDMov video container (MOV-wrapped streams of GPR-encoded frames).
 *
 * GVIDMov is a single-file, frame-independent, metadata-rich video container
 * for streams of fused-format GPR-encoded frames. It is an ISO BMFF/MOV
 * compatibility wrapper carrying a custom codec_tag, one AVPacket per video
 * frame, plus per-track and per-frame metadata. The primary raw-video capture
 * deliverable is the neutral .gvid stream.
 *
 * ## Wire format
 *
 *   Container        : ISO BMFF / MOV  (.mov extension)
 *   Video codec_tag  : 'GPRr'  (0x47 0x50 0x52 0x72)
 *   AVCodecID        : AV_CODEC_ID_NONE  (tag-only; failure if rejected)
 *   Frame payload    : opaque GPR bytes — FUSED_HEADER + band table + bands
 *                      (exactly the buffer gpr_encode_fused_frame produces).
 *   Frame timing     : strictly monotonic PTS/DTS at the requested fps.
 *
 * ## Track structure
 *
 *   - 1 video track (codec_tag = GPRr, mandatory)
 *   - audio + timecode tracks reserved but not written in this revision
 *
 * ## Track-level metadata (MOV moov udta, written via AVStream.metadata)
 *
 *   gpr.codec_version       e.g. "vc5/2.0+gpr"
 *   gpr.quality             0..5
 *   gpr.cfa_pattern         "RGGB", "GBRG", ...
 *   gpr.bit_depth           14 or 16
 *   gpr.black_level         integer (min channel)
 *   gpr.white_level         integer
 *   gpr.encoder_settings    JSON blob, env-var equivalents
 *   gpr.source_dng_path     optional traceability
 *   gpr.color_matrix        9 comma-separated floats (XYZ if known)
 *
 * ## Per-frame metadata
 *
 *   Passed to the muxer as AV_PKT_DATA_STRINGS_METADATA. This API does not
 *   guarantee MOV persistence or expose per-frame metadata on readback.
 *
 *   gpr.frame_timestamp     nanoseconds from start
 *   gpr.iso                 ISO speed
 *   gpr.shutter             shutter angle (degrees)
 *   gpr.wb_neutral          "R,G,B" as-shot WB neutrals
 *
 * (C) 2026 Happy. Apache-2.0 or MIT — match repo policy.
 */

#ifndef GVID_MOV_H
#define GVID_MOV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The codec_tag four-character code used in the MOV video sample
   description. Little-endian uint32 spelling 'GPRr' on the wire. */
#define GVID_MOV_CODEC_TAG  (((uint32_t)'G') | ((uint32_t)'P' << 8) | \
                          ((uint32_t)'R' << 16) | ((uint32_t)'r' << 24))

/* Track-level metadata. All char* fields may be NULL to skip writing them.
   Numeric fields with value 0 are written as "0" (the caller is responsible
   for leaving them at -1 or omitting via a wrapper if "unknown" must be
   distinct from zero). */
typedef struct {
    const char *codec_version;      /* e.g. "vc5/2.0+gpr" */
    int         quality;            /* 0..5 (negative = skip) */
    const char *cfa_pattern;        /* "RGGB" */
    int         bit_depth;          /* 14 or 16 (<=0 = skip) */
    int         black_level;        /* min-channel black (<0 = skip) */
    int         white_level;        /* (<0 = skip) */
    const char *encoder_settings;   /* JSON blob */
    const char *source_dng_path;    /* original path, optional */
    const char *color_matrix;       /* "m00,m01,...,m22" or NULL */
} GVIDMov_Metadata;

/* Per-frame metadata. NULL/negative fields are not written. */
typedef struct {
    int64_t  frame_timestamp_ns;    /* override; if <0, derive from PTS */
    int      iso;                   /* <0 = skip */
    double   shutter_deg;           /* <0 = skip */
    const char *wb_neutral;         /* "R,G,B" or NULL */
} GVIDMov_FrameMeta;

typedef struct GVIDMov_Writer GVIDMov_Writer;
typedef struct GVIDMov_Reader GVIDMov_Reader;

/* -------- Writer -------- */

/* Create a GVIDMov writer. width/height should be the OUTPUT (decoded) bayer
   dimensions — for LL-only-fast that is the half-res target. fps_num/fps_den
   set the time base; pass e.g. (24, 1) or (30000, 1001).
   meta may be NULL.
   Returns NULL on failure (stderr explains). */
GVIDMov_Writer *gvid_mov_writer_create(const char *path,
                                  int width, int height,
                                  int fps_num, int fps_den,
                                  const GVIDMov_Metadata *meta);

/* Append one frame. The payload is whatever fused-format byte buffer the
   encoder produced (FUSED_MAGIC at offset 0). timestamp_ns is informational;
   actual MOV PTS comes from the frame index × (fps_den / fps_num).
   fm may be NULL. Returns 0 on success, negative on error. */
int gvid_mov_writer_add_frame(GVIDMov_Writer *w,
                           const uint8_t *gpr_bytes, size_t n,
                           int64_t timestamp_ns,
                           const GVIDMov_FrameMeta *fm);

/* Finalize the moov atom and close the file. Frees the writer. */
int gvid_mov_writer_close(GVIDMov_Writer *w);

/* -------- Reader -------- */

GVIDMov_Reader *gvid_mov_reader_open(const char *path);

/* Copy out the track-level metadata. Strings are owned by the reader and
   remain valid until gvid_mov_reader_close(). */
int gvid_mov_reader_get_metadata(GVIDMov_Reader *r, GVIDMov_Metadata *meta);

/* Stream width/height/fps from the video stream. */
int gvid_mov_reader_get_video_info(GVIDMov_Reader *r,
                                int *width, int *height,
                                int *fps_num, int *fps_den,
                                int64_t *frame_count);

/* Describe companion tracks; audio is not decoded by this payload reader. */
int gvid_mov_reader_get_aux_info(GVIDMov_Reader *r, int *has_audio,
                               int *has_timecode, char *timecode, size_t capacity);

/* Read the next packet. *gpr_bytes points into reader-owned memory that is
   valid until the NEXT call to gvid_mov_reader_next_frame (or _close).
   Returns 0 on success, AVERROR_EOF when no more frames, negative on error. */
int gvid_mov_reader_next_frame(GVIDMov_Reader *r,
                            const uint8_t **gpr_bytes, size_t *n,
                            int64_t *timestamp_ns);

int gvid_mov_reader_close(GVIDMov_Reader *r);

#ifdef __cplusplus
}
#endif

#endif /* GVID_MOV_H */
