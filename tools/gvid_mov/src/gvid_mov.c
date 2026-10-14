/* gvid_mov.c — GVIDMov MOV container reader/writer.
 *
 * Implemented against FFmpeg 8.x (libavformat 62, libavutil 60). The MOV
 * muxer accepts a custom codec_tag without any patching: we set
 * AVCodecParameters.codec_id = AV_CODEC_ID_NONE and codec_tag = 'GPRr',
 * and the demuxer round-trips both values.
 *
 * Why AV_CODEC_ID_NONE works in FFmpeg 8: the MOV muxer treats unknown
 * codec_ids as opaque "raw" video samples — it writes a stsd entry with
 * the codec_tag we hand it and copies the AVPacket payload verbatim into
 * mdat. The demuxer mirrors this. Unsupported muxer versions fail rather
 * than mislabeling the payload as an unrelated codec.
 */

#define _POSIX_C_SOURCE 200809L
#include "gvid_mov.h"

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ---------- common helpers ---------- */

static void av_log_err(const char *what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    fprintf(stderr, "gvid_mov: %s failed: %s (%d)\n", what, buf, err);
}

/* Set k=v in dict if v != NULL. */
static void dict_set_str(AVDictionary **d, const char *k, const char *v) {
    if (v) av_dict_set(d, k, v, 0);
}

static void dict_set_int(AVDictionary **d, const char *k, int64_t v) {
    av_dict_set_int(d, k, v, 0);
}

/* ---------- Writer ---------- */

struct GVIDMov_Writer {
    AVFormatContext *oc;
    AVStream        *video;
    int              fps_num;
    int              fps_den;
    int64_t          frame_index;
    int              header_written;
};

static int writer_set_track_metadata(AVStream *st, const GVIDMov_Metadata *m) {
    AVDictionary **d = &st->metadata;
    if (!m) return 0;
    dict_set_str(d, "gpr.codec_version",    m->codec_version);
    if (m->quality >= 0)     dict_set_int(d, "gpr.quality",     m->quality);
    dict_set_str(d, "gpr.cfa_pattern",      m->cfa_pattern);
    if (m->bit_depth > 0)    dict_set_int(d, "gpr.bit_depth",   m->bit_depth);
    if (m->black_level >= 0) dict_set_int(d, "gpr.black_level", m->black_level);
    if (m->white_level >= 0) dict_set_int(d, "gpr.white_level", m->white_level);
    dict_set_str(d, "gpr.encoder_settings", m->encoder_settings);
    dict_set_str(d, "gpr.source_dng_path",  m->source_dng_path);
    dict_set_str(d, "gpr.color_matrix",     m->color_matrix);
    return 0;
}

GVIDMov_Writer *gvid_mov_writer_create(const char *path,
                                  int width, int height,
                                  int fps_num, int fps_den,
                                  const GVIDMov_Metadata *meta) {
    if (!path || width <= 0 || height <= 0 || fps_num <= 0 || fps_den <= 0) {
        fprintf(stderr, "gvid_mov: bad writer args\n");
        return NULL;
    }
    GVIDMov_Writer *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->fps_num = fps_num;
    w->fps_den = fps_den;

    int rc = avformat_alloc_output_context2(&w->oc, NULL, "mov", path);
    if (rc < 0 || !w->oc) { av_log_err("alloc_output_context2", rc); free(w); return NULL; }

    w->video = avformat_new_stream(w->oc, NULL);
    if (!w->video) { fprintf(stderr, "gvid_mov: new_stream failed\n");
                     avformat_free_context(w->oc); free(w); return NULL; }

    AVCodecParameters *p = w->video->codecpar;
    p->codec_type = AVMEDIA_TYPE_VIDEO;
    p->codec_id   = AV_CODEC_ID_NONE;
    p->codec_tag  = GVID_MOV_CODEC_TAG;     /* 'GPRr' */
    p->width      = width;
    p->height     = height;
    p->format     = AV_PIX_FMT_NONE;     /* opaque — codec_tag identifies it */

    /* Stream time base = 1 / fps_num (so PTS increments by fps_den per frame). */
    w->video->time_base       = (AVRational){ fps_den, fps_num };
    w->video->avg_frame_rate  = (AVRational){ fps_num, fps_den };
    w->video->r_frame_rate    = (AVRational){ fps_num, fps_den };

    writer_set_track_metadata(w->video, meta);
    /* MOV's per-stream metadata writer is restrictive (it whitelists a few
       known QuickTime/iTunes keys and drops the rest). To make sure the
       gpr.* keys survive the round trip we also stash them on the format
       context — written into the file-level mdta atom when
       movflags=use_metadata_tags is set. The reader pulls from the stream
       first and falls back to the file. */
    {
        AVDictionary **fd = &w->oc->metadata;
        if (meta) {
            dict_set_str(fd, "gpr.codec_version",    meta->codec_version);
            if (meta->quality >= 0)     dict_set_int(fd, "gpr.quality",     meta->quality);
            dict_set_str(fd, "gpr.cfa_pattern",      meta->cfa_pattern);
            if (meta->bit_depth > 0)    dict_set_int(fd, "gpr.bit_depth",   meta->bit_depth);
            if (meta->black_level >= 0) dict_set_int(fd, "gpr.black_level", meta->black_level);
            if (meta->white_level >= 0) dict_set_int(fd, "gpr.white_level", meta->white_level);
            dict_set_str(fd, "gpr.encoder_settings", meta->encoder_settings);
            dict_set_str(fd, "gpr.source_dng_path",  meta->source_dng_path);
            dict_set_str(fd, "gpr.color_matrix",     meta->color_matrix);
        }
    }

    if (!(w->oc->oformat->flags & AVFMT_NOFILE)) {
        rc = avio_open(&w->oc->pb, path, AVIO_FLAG_WRITE);
        if (rc < 0) { av_log_err("avio_open", rc); avformat_free_context(w->oc); free(w); return NULL; }
    }

    /* faststart — moves moov to the front. Optional but cheap and lets
       readers skip a final-seek on close.
       use_metadata_tags — writes the mdta atom so arbitrary gpr.* keys
       survive a round trip (otherwise MOV silently drops unknown keys). */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "movflags", "+faststart+use_metadata_tags", 0);

    rc = avformat_write_header(w->oc, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        av_log_err("write_header", rc);
        if (w->oc->pb) avio_closep(&w->oc->pb);
        avformat_free_context(w->oc);
        free(w);
        return NULL;
    }
    w->header_written = 1;
    return w;
}

static int writer_attach_frame_meta(AVPacket *pkt, const GVIDMov_FrameMeta *fm,
                                    int64_t derived_ns) {
    if (!fm && derived_ns < 0) return 0;

    AVDictionary *metadata = NULL;
    int64_t ts = (fm && fm->frame_timestamp_ns >= 0) ? fm->frame_timestamp_ns
                                                     : derived_ns;
    if (ts >= 0) {
        av_dict_set_int(&metadata, "gpr.frame_timestamp", ts, 0);
    }
    if (fm) {
        if (fm->iso >= 0)
            av_dict_set_int(&metadata, "gpr.iso", fm->iso, 0);
        if (fm->shutter_deg >= 0) {
            char shutter[64];
            snprintf(shutter, sizeof(shutter), "%.3f", fm->shutter_deg);
            av_dict_set(&metadata, "gpr.shutter", shutter, 0);
        }
        if (fm->wb_neutral)
            av_dict_set(&metadata, "gpr.wb_neutral", fm->wb_neutral, 0);
    }
    if (!metadata) return 0;
    size_t size = 0;
    uint8_t *side = av_packet_pack_dictionary(metadata, &size);
    av_dict_free(&metadata);
    if (!side) return AVERROR(ENOMEM);
    int rc = av_packet_add_side_data(pkt, AV_PKT_DATA_STRINGS_METADATA, side, size);
    if (rc < 0) av_free(side);
    return rc;
}

int gvid_mov_writer_add_frame(GVIDMov_Writer *w,
                           const uint8_t *gpr_bytes, size_t n,
                           int64_t timestamp_ns,
                           const GVIDMov_FrameMeta *fm) {
    if (!w || !gpr_bytes || n == 0 || n > INT_MAX) return -1;

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return AVERROR(ENOMEM);

    int rc = av_new_packet(pkt, (int)n);
    if (rc < 0) { av_packet_free(&pkt); av_log_err("av_new_packet", rc); return rc; }
    memcpy(pkt->data, gpr_bytes, n);

    /* After avformat_write_header the MOV muxer may have substituted a
       finer timescale (e.g. 12288 instead of 24). Rescale our integer
       frame index into the current stream time_base so PTS expresses
       "frame_index seconds of (fps_den/fps_num)" in either case. */
    AVRational src_tb = (AVRational){ w->fps_den, w->fps_num };
    int64_t    pts    = av_rescale_q(w->frame_index, src_tb, w->video->time_base);
    int64_t    dur    = av_rescale_q(1,              src_tb, w->video->time_base);

    pkt->stream_index = w->video->index;
    pkt->pts          = pts;
    pkt->dts          = pts;
    pkt->duration     = dur > 0 ? dur : 1;
    pkt->flags        = AV_PKT_FLAG_KEY;           /* every GPR frame is independent */

    /* Optional frame-level side data. */
    int64_t derived_ns = -1;
    if (timestamp_ns >= 0) derived_ns = timestamp_ns;
    else                   derived_ns = av_rescale(w->frame_index,
                                                   (int64_t)w->fps_den * 1000000000LL,
                                                   w->fps_num);
    rc = writer_attach_frame_meta(pkt, fm, derived_ns);
    if (rc < 0) { av_packet_free(&pkt); return rc; }

    rc = av_write_frame(w->oc, pkt);
    av_packet_free(&pkt);
    if (rc < 0) { av_log_err("av_write_frame", rc); return rc; }
    w->frame_index++;
    return 0;
}

int gvid_mov_writer_close(GVIDMov_Writer *w) {
    if (!w) return -1;
    int rc = 0;
    if (w->header_written) {
        rc = av_write_trailer(w->oc);
        if (rc < 0) av_log_err("av_write_trailer", rc);
    }
    if (w->oc) {
        if (w->oc->pb) {
            int close_rc = avio_closep(&w->oc->pb);
            if (rc >= 0) rc = close_rc;
        }
        avformat_free_context(w->oc);
    }
    free(w);
    return rc;
}

/* ---------- Reader ---------- */

struct GVIDMov_Reader {
    AVFormatContext *ic;
    int              video_idx;
    AVPacket        *pkt;       /* reused; ref'd into demuxer-owned memory */

    /* Cached metadata strings copied from AVStream.metadata so the API
       contract ("valid until reader_close") stays simple. */
    char *codec_version;
    char *cfa_pattern;
    char *encoder_settings;
    char *source_dng_path;
    char *color_matrix;
    GVIDMov_Metadata meta_cache;
};

static char *dup_dict_str(const AVDictionary *d, const char *key) {
    AVDictionaryEntry *e = av_dict_get(d, key, NULL, 0);
    if (!e || !e->value) return NULL;
    return strdup(e->value);
}

GVIDMov_Reader *gvid_mov_reader_open(const char *path) {
    if (!path) return NULL;
    GVIDMov_Reader *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->video_idx = -1;

    int rc = avformat_open_input(&r->ic, path, NULL, NULL);
    if (rc < 0) { av_log_err("open_input", rc); free(r); return NULL; }

    rc = avformat_find_stream_info(r->ic, NULL);
    if (rc < 0) { av_log_err("find_stream_info", rc);
                  avformat_close_input(&r->ic); free(r); return NULL; }

    for (unsigned i = 0; i < r->ic->nb_streams; i++) {
        AVStream *s = r->ic->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (s->codecpar->codec_tag == GVID_MOV_CODEC_TAG ||
                s->codecpar->codec_tag == MKTAG('G', 'P', 'R', '1')) {
                r->video_idx = (int)i;
                break;
            }
        }
    }
    if (r->video_idx < 0) {
        fprintf(stderr, "gvid_mov: no GPRr/GPR1 video stream in %s\n", path);
        avformat_close_input(&r->ic); free(r); return NULL;
    }

    AVStream *vs = r->ic->streams[r->video_idx];

    r->pkt = av_packet_alloc();
    if (!r->pkt) { avformat_close_input(&r->ic); free(r); return NULL; }

    /* Cache metadata strings. MOV's stream-level metadata is restrictive;
       most gpr.* keys come back via the file-level mdta atom. Probe both
       and prefer whichever has a value. */
    AVDictionary *sd = vs->metadata;
    AVDictionary *fd = r->ic->metadata;
    #define DUP(K) ({ char *v = dup_dict_str(sd, K); if (!v) v = dup_dict_str(fd, K); v; })
    r->codec_version    = DUP("gpr.codec_version");
    r->cfa_pattern      = DUP("gpr.cfa_pattern");
    r->encoder_settings = DUP("gpr.encoder_settings");
    r->source_dng_path  = DUP("gpr.source_dng_path");
    r->color_matrix     = DUP("gpr.color_matrix");
    #undef DUP

    #define IGI(K, D)  ({ \
        AVDictionaryEntry *e = av_dict_get(sd, K, NULL, 0); \
        if (!e) e = av_dict_get(fd, K, NULL, 0); \
        e && e->value ? atoi(e->value) : (D); \
    })
    r->meta_cache.codec_version    = r->codec_version;
    r->meta_cache.quality          = IGI("gpr.quality",     -1);
    r->meta_cache.cfa_pattern      = r->cfa_pattern;
    r->meta_cache.bit_depth        = IGI("gpr.bit_depth",    0);
    r->meta_cache.black_level      = IGI("gpr.black_level", -1);
    r->meta_cache.white_level      = IGI("gpr.white_level", -1);
    #undef IGI
    r->meta_cache.encoder_settings = r->encoder_settings;
    r->meta_cache.source_dng_path  = r->source_dng_path;
    r->meta_cache.color_matrix     = r->color_matrix;
    return r;
}

int gvid_mov_reader_get_metadata(GVIDMov_Reader *r, GVIDMov_Metadata *meta) {
    if (!r || !meta) return -1;
    *meta = r->meta_cache;
    return 0;
}

int gvid_mov_reader_get_video_info(GVIDMov_Reader *r,
                                int *width, int *height,
                                int *fps_num, int *fps_den,
                                int64_t *frame_count) {
    if (!r) return -1;
    AVStream *vs = r->ic->streams[r->video_idx];
    if (width)  *width  = vs->codecpar->width;
    if (height) *height = vs->codecpar->height;
    if (fps_num || fps_den) {
        AVRational fr = vs->avg_frame_rate;
        if (fr.num == 0) fr = vs->r_frame_rate;
        if (fps_num) *fps_num = fr.num;
        if (fps_den) *fps_den = fr.den ? fr.den : 1;
    }
    if (frame_count) *frame_count = vs->nb_frames;
    return 0;
}

int gvid_mov_reader_get_aux_info(GVIDMov_Reader *r, int *has_audio,
                               int *has_timecode, char *timecode, size_t capacity) {
    if (!r) return -1;
    if (has_audio) *has_audio = 0;
    if (has_timecode) *has_timecode = 0;
    if (timecode && capacity) timecode[0] = '\0';
    for (unsigned i = 0; i < r->ic->nb_streams; ++i) {
        AVStream *stream = r->ic->streams[i];
        if (has_audio && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            *has_audio = 1;
        if (stream->codecpar->codec_tag == MKTAG('t', 'm', 'c', 'd')) {
            if (has_timecode) *has_timecode = 1;
            AVDictionaryEntry *entry = av_dict_get(stream->metadata, "timecode", NULL, 0);
            if (entry && timecode && capacity)
                snprintf(timecode, capacity, "%s", entry->value);
        }
    }
    return 0;
}

int gvid_mov_reader_next_frame(GVIDMov_Reader *r,
                            const uint8_t **gpr_bytes, size_t *n,
                            int64_t *timestamp_ns) {
    if (!r) return -1;
    av_packet_unref(r->pkt);
    while (1) {
        int rc = av_read_frame(r->ic, r->pkt);
        if (rc == AVERROR_EOF) return AVERROR_EOF;
        if (rc < 0) { av_log_err("read_frame", rc); return rc; }
        if (r->pkt->stream_index != r->video_idx) {
            av_packet_unref(r->pkt);
            continue;
        }
        if ((r->pkt->flags & AV_PKT_FLAG_CORRUPT) || r->pkt->size <= 0) {
            av_packet_unref(r->pkt);
            return AVERROR_INVALIDDATA;
        }
        if (gpr_bytes) *gpr_bytes = r->pkt->data;
        if (n)         *n         = (size_t)r->pkt->size;
        if (timestamp_ns) {
            AVStream *vs = r->ic->streams[r->video_idx];
            int64_t pts = r->pkt->pts != AV_NOPTS_VALUE ? r->pkt->pts : 0;
            *timestamp_ns = av_rescale_q(pts, vs->time_base,
                                         (AVRational){1, 1000000000});
        }
        return 0;
    }
}

int gvid_mov_reader_close(GVIDMov_Reader *r) {
    if (!r) return -1;
    if (r->pkt) av_packet_free(&r->pkt);
    if (r->ic)  avformat_close_input(&r->ic);
    free(r->codec_version);
    free(r->cfa_pattern);
    free(r->encoder_settings);
    free(r->source_dng_path);
    free(r->color_matrix);
    free(r);
    return 0;
}
