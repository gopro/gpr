/*
 * GPR (GoPro RAW / fused-VC5) video decoder for libavcodec.
 *
 * Copyright (c) 2026 Happy.
 *
 * This file is part of FFmpeg (out-of-tree patch — installed into
 * libavcodec/gpr.c by tools/gvid_codec/install_patch.sh).
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/**
 * @file
 * GPR fused-format decoder. One AVPacket == one self-contained GPR frame
 * (FUSED_HEADER + band manifest + rANS bands). This decoder is a thin
 * wrapper around libvc5_decoder's gpr_decode_fused(): we hand it the
 * packet bytes and it writes a 16-bit Bayer plane.
 *
 * Output pixel format: AV_PIX_FMT_BAYER_RGGB16LE or
 * AV_PIX_FMT_BAYER_GBRG16LE depending on the FUSED_HEADER.is_rggb flag.
 * (FFmpeg's two BAYER pixel formats use bit-packed 16-bit single-plane
 * layout — the same shape gpr_decode_fused writes.)
 *
 * Stream identification: codec_tag 'GPRr' (little-endian uint32 spelling
 * G, P, R, r), mapped in libavformat/isom_tags.c.
 */

/* FUSED_HEADER layout (must mirror source/lib/vc5_encoder/fused_encode.h).
 * All fields are little-endian uint32_t. We re-state them here so this
 * file has no source-tree include path dependency beyond libvc5_decoder.a. */
#define GPR_FUSED_MAGIC      0x44535546u   /* 'FUSD' LE                 */
#define GPR_FUSED_HDR_SIZE   48u           /* 12 uint32_t fields        */
#define GPR_FUSED_OFF_MAGIC   0
#define GPR_FUSED_OFF_VER     4
#define GPR_FUSED_OFF_WIDTH   8
#define GPR_FUSED_OFF_HEIGHT 12
#define GPR_FUSED_OFF_PIXFMT 16
#define GPR_FUSED_OFF_QUAL   20
#define GPR_FUSED_OFF_RGGB   24
#define GPR_FUSED_OFF_LOGB   28
#define GPR_FUSED_OFF_PRES   32
#define GPR_FUSED_OFF_MULTI  36
#define GPR_FUSED_OFF_BANDS  40
#define GPR_FUSED_OFF_DECIM  44

#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

/* External decoder API from libvc5_decoder. We deliberately forward-declare
   here rather than including fused_decode.h so this file stays inside the
   FFmpeg source tree without an include-path entanglement. */
int gpr_decode_fused(const uint8_t *enc, size_t enc_size,
                     uint16_t *bayer_out, size_t bayer_pitch_bytes,
                     int *out_width, int *out_height);

typedef struct GPRContext {
    AVClass *class;
    /* Decoded dimensions latched on first frame, validated thereafter. */
    int latched_width;
    int latched_height;
} GPRContext;

static av_cold int gpr_dec_init(AVCodecContext *avctx)
{
    GPRContext *s = avctx->priv_data;
    s->latched_width  = 0;
    s->latched_height = 0;

    /* CFA and sample depth are packet properties. Leave them unknown so
       avformat_find_stream_info must decode a packet instead of accepting
       a guessed format from decoder initialization or the container. */
    avctx->pix_fmt = AV_PIX_FMT_NONE;
    avctx->bits_per_raw_sample = 0;
    return 0;
}

static av_cold int gpr_dec_close(AVCodecContext *avctx)
{
    return 0;
}

static int gpr_dec_decode(AVCodecContext *avctx, AVFrame *frame,
                          int *got_frame, AVPacket *avpkt)
{
    GPRContext *s = avctx->priv_data;
    int width = 0, height = 0;
    uint32_t w, h, pf, levels, bands, bits, prescale, is_rggb, decim;
    size_t off;
    int ret;

    *got_frame = 0;

    if (!avpkt || !avpkt->data || avpkt->size < (int)GPR_FUSED_HDR_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "gpr: packet too small (%d bytes)\n",
               avpkt ? avpkt->size : 0);
        return AVERROR_INVALIDDATA;
    }

    /* Validate magic + peek width/height/CFA from the FUSED_HEADER. */
    if (AV_RL32(avpkt->data + GPR_FUSED_OFF_MAGIC) != GPR_FUSED_MAGIC) {
        av_log(avctx, AV_LOG_ERROR,
               "gpr: bad magic 0x%08x (expected 0x%08x)\n",
               AV_RL32(avpkt->data + GPR_FUSED_OFF_MAGIC), GPR_FUSED_MAGIC);
        return AVERROR_INVALIDDATA;
    }

    w = AV_RL32(avpkt->data + GPR_FUSED_OFF_WIDTH);
    h = AV_RL32(avpkt->data + GPR_FUSED_OFF_HEIGHT);
    pf = AV_RL32(avpkt->data + GPR_FUSED_OFF_PIXFMT);
    levels = AV_RL32(avpkt->data + GPR_FUSED_OFF_MULTI);
    bands = AV_RL32(avpkt->data + GPR_FUSED_OFF_BANDS);
    bits = AV_RL32(avpkt->data + GPR_FUSED_OFF_LOGB);
    prescale = AV_RL32(avpkt->data + GPR_FUSED_OFF_PRES);
    is_rggb = AV_RL32(avpkt->data + GPR_FUSED_OFF_RGGB);
    decim = AV_RL32(avpkt->data + GPR_FUSED_OFF_DECIM);
    if (AV_RL32(avpkt->data + GPR_FUSED_OFF_VER) != 1 ||
        AV_RL32(avpkt->data + GPR_FUSED_OFF_QUAL) >= 12 ||
        pf > 5 || is_rggb > 1 ||
        is_rggb != (pf == 0 || pf == 1 || pf == 4) ||
        (bits != 12 && bits != 14 && bits != 16) ||
        (prescale != 0 && prescale != 2 && prescale != 3) ||
        !((levels == 0 && bands == 16) ||
          (levels == 2 && bands == 28) ||
          ((levels == 1 || levels == 3) && bands == 40)) ||
        w > 16384 || h > 16384)
        goto invalid_header;

    /* Match gpr_decode_fused: only exactly 2 denotes decimation. */
    width = w / (decim == 2 ? 2 : 1);
    height = h / (decim == 2 ? 2 : 1);
    if (width < 16 || height < 16 || (width & 3) || (height & 3))
        goto invalid_header;

    /* The multi-level decoder copies this table without checking its size.
       Check every span with subtraction to avoid addition overflow. Zero
       and short bands are valid implicit-zero bands in the fused decoder. */
    off = GPR_FUSED_HDR_SIZE + 4 * bands;
    if (off > (size_t)avpkt->size)
        goto invalid_header;
    for (unsigned i = 0; i < bands; i++) {
        uint32_t size = AV_RL32(avpkt->data + GPR_FUSED_HDR_SIZE + 4 * i);
        if (size > (size_t)avpkt->size - off)
            goto invalid_header;
        off += size;
    }
    if (off != (size_t)avpkt->size)
        goto invalid_header;
    enum AVPixelFormat want_fmt = is_rggb ? AV_PIX_FMT_BAYER_RGGB16LE
                                          : AV_PIX_FMT_BAYER_GBRG16LE;
    if (avctx->pix_fmt != want_fmt) {
        avctx->pix_fmt = want_fmt;
    }

    avctx->bits_per_raw_sample = bits;

    /* A container can advertise smaller dimensions than the packet needs. */
    if (!s->latched_width) {
        s->latched_width  = width;
        s->latched_height = height;
    } else if (width != s->latched_width || height != s->latched_height) {
        av_log(avctx, AV_LOG_ERROR,
               "gpr: dimension change mid-stream not supported "
               "(%dx%d -> %dx%d)\n",
               s->latched_width, s->latched_height, width, height);
        return AVERROR_INVALIDDATA;
    }
    if ((ret = ff_set_dimensions(avctx, width, height)) < 0)
        return ret;

    /* ff_get_buffer only fills an unset frame format/dimensions. Replace
       any properties inherited from the container or previous packet. */
    frame->format = avctx->pix_fmt;
    frame->width  = width;
    frame->height = height;

    /* Allocate the output frame. Bayer 16-bit is one component per sample,
       so linesize[0] = width * 2 bytes. */
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;

    /* Hand the packet straight to the GPR decoder. It writes directly
       into the AVFrame data buffer at our requested stride. */
    int decoded_w = 0, decoded_h = 0;
    ret = gpr_decode_fused(avpkt->data, (size_t)avpkt->size,
                           (uint16_t *)frame->data[0],
                           (size_t)frame->linesize[0],
                           &decoded_w, &decoded_h);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR,
               "gpr: gpr_decode_fused failed: %d\n", ret);
        return AVERROR_INVALIDDATA;
    }
    if (decoded_w != width || decoded_h != height) {
        av_log(avctx, AV_LOG_ERROR,
               "gpr: header/decode dim mismatch %dx%d vs %dx%d\n",
               width, height, decoded_w, decoded_h);
        return AVERROR_INVALIDDATA;
    }

    frame->pict_type = AV_PICTURE_TYPE_I;     /* every GPR frame is intra */
    frame->flags    |= AV_FRAME_FLAG_KEY;
    frame->pts       = avpkt->pts;
    *got_frame = 1;
    return avpkt->size;

invalid_header:
    av_log(avctx, AV_LOG_ERROR, "gpr: invalid FUSD header or band bounds\n");
    return AVERROR_INVALIDDATA;
}

const FFCodec ff_gpr_decoder = {
    .p.name           = "gpr",
    CODEC_LONG_NAME("GoPro RAW (fused VC-5)"),
    .p.type           = AVMEDIA_TYPE_VIDEO,
    .p.id             = AV_CODEC_ID_GPR,
    .priv_data_size   = sizeof(GPRContext),
    .init             = gpr_dec_init,
    .close            = gpr_dec_close,
    FF_CODEC_DECODE_CB(gpr_dec_decode),
    .p.capabilities   = AV_CODEC_CAP_DR1,
    .caps_internal    = FF_CODEC_CAP_INIT_CLEANUP,
};
