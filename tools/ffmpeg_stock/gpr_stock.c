/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/* Optional out-of-tree FFmpeg n8.0 adapter for framed stock GVID MOV samples. */
#include "libavutil/intreadwrite.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "gpr_gvid_reader.h"
#include "gpr_preview.h"
#include "vc5_decoder.h"

typedef struct {
    gpr_gvid_config video;
    unsigned version;
    VC5_DECODER_PIXEL_FORMAT format;
} GPRStockContext;
typedef struct {const uint8_t *bytes;size_t size,position;} Prefix;
static ptrdiff_t prefix_read(void *opaque,uint8_t *out,size_t size)
{
    Prefix *p=opaque;
    size_t count=FFMIN(size,p->size-p->position);
    memcpy(out,p->bytes+p->position,count);p->position+=count;return count;
}
static av_cold int stock_init(AVCodecContext *avctx)
{
    GPRStockContext *s=avctx->priv_data;
    gpr_gvid_reader *reader=NULL;gpr_gvid_stream_info info;
    Prefix prefix={avctx->extradata,avctx->extradata_size,0};
    unsigned phase=0,depth;
    static const VC5_DECODER_PIXEL_FORMAT formats[4][3]={
        {VC5_DECODER_PIXEL_FORMAT_RGGB_12,VC5_DECODER_PIXEL_FORMAT_RGGB_14,VC5_DECODER_PIXEL_FORMAT_RGGB_16},
        {VC5_DECODER_PIXEL_FORMAT_GBRG_12,VC5_DECODER_PIXEL_FORMAT_GBRG_14,VC5_DECODER_PIXEL_FORMAT_GBRG_16},
        {VC5_DECODER_PIXEL_FORMAT_GRBG_12,VC5_DECODER_PIXEL_FORMAT_GRBG_14,VC5_DECODER_PIXEL_FORMAT_GRBG_16},
        {VC5_DECODER_PIXEL_FORMAT_BGGR_12,VC5_DECODER_PIXEL_FORMAT_BGGR_14,VC5_DECODER_PIXEL_FORMAT_BGGR_16}};
    static const enum AVPixelFormat pixels[4]={AV_PIX_FMT_BAYER_RGGB16LE,AV_PIX_FMT_BAYER_GBRG16LE,
        AV_PIX_FMT_BAYER_GRBG16LE,AV_PIX_FMT_BAYER_BGGR16LE};
    if (!avctx->extradata || avctx->extradata_size<64 || avctx->extradata_size>64+65536 ||
        gpr_gvid_reader_create(prefix_read,&prefix,0,&reader)) return AVERROR_INVALIDDATA;
    int error=gpr_gvid_reader_info(reader,&info);
    if (!error) {s->video=info.video;s->version=info.version;}
    gpr_gvid_reader_close(reader);
    if (error || prefix.position!=prefix.size || s->video.width<48 || s->video.height<48 ||
        (uint64_t)s->video.width*s->video.height>INT_MAX/2 ||
        (avctx->width && avctx->width!=s->video.width) || (avctx->height && avctx->height!=s->video.height) ||
        avctx->codec_tag!=(s->version==3 ? MKTAG('G','V','S','3'):MKTAG('G','V','S','2')))
        return AVERROR_INVALIDDATA;
    if (!memcmp(s->video.cfa,"GBRG",4)) phase=1;
    else if (!memcmp(s->video.cfa,"GRBG",4)) phase=2;
    else if (!memcmp(s->video.cfa,"BGGR",4)) phase=3;
    depth=(s->video.sample_bits-12)/2;s->format=formats[phase][depth];
    avctx->pix_fmt=pixels[phase];avctx->bits_per_raw_sample=s->video.sample_bits;
    return ff_set_dimensions(avctx,s->video.width,s->video.height);
}
static int stock_decode(AVCodecContext *avctx,AVFrame *frame,int *got_frame,AVPacket *packet)
{
    GPRStockContext *s=avctx->priv_data;
    unsigned header=s->version==3 ? 48:32;
    size_t metadata=0,bytes;
    const uint8_t *payload;
    uint8_t *aligned=NULL;
    gpr_buffer input,raw={0};vc5_decoder_parameters parameters;
    int error;
    *got_frame=0;
    if (packet->size<(int)header || !packet->data || (packet->flags&AV_PKT_FLAG_CORRUPT) ||
        memcmp(packet->data,s->version==3 ? "FRM3":"FRM2",4) || AV_RL32(packet->data+4)!=header)
        return AVERROR_INVALIDDATA;
    if (s->version==3) {
        metadata=AV_RL32(packet->data+32);
        if (metadata>65536 || AV_RL32(packet->data+44)!=gpr_gvid_crc32(0,packet->data,44)) return AVERROR_INVALIDDATA;
    }
    if (metadata>packet->size-header || AV_RL64(packet->data+24)!=(uint64_t)packet->size-header-metadata)
        return AVERROR_INVALIDDATA;
    bytes=packet->size-header-metadata;payload=packet->data+header+metadata;
    if (!bytes || bytes>128u*1024*1024) return AVERROR_INVALIDDATA;
    if (s->version==3 && (AV_RL32(packet->data+36)!=gpr_gvid_crc32(0,payload,bytes) ||
        AV_RL32(packet->data+40)!=gpr_gvid_crc32(0,packet->data+header,metadata))) return AVERROR_INVALIDDATA;
    if (gpr_preview_validate_payload(&s->video,payload,bytes)) return AVERROR_INVALIDDATA;
    aligned=av_mallocz(bytes+AV_INPUT_BUFFER_PADDING_SIZE);
    if (!aligned) return AVERROR(ENOMEM);
    memcpy(aligned,payload,bytes);input.buffer=aligned;input.size=bytes;
    vc5_decoder_parameters_set_default(&parameters);
    parameters.mem_alloc=av_malloc;parameters.mem_free=av_free;parameters.pixel_format=s->format;
    error=vc5_decoder_process(&parameters,&input,&raw,NULL);av_free(aligned);
    if (error || !raw.buffer || raw.size!=(size_t)s->video.width*s->video.height*2) {
        av_free(raw.buffer);return error==CODEC_ERROR_OUTOFMEMORY ? AVERROR(ENOMEM):AVERROR_INVALIDDATA;
    }
    error=ff_get_buffer(avctx,frame,0);
    if (error<0) {av_free(raw.buffer);return error;}
    for (unsigned y=0;y<s->video.height;++y) {
#if HAVE_BIGENDIAN
        const uint16_t *row=(const uint16_t *)raw.buffer+(size_t)y*s->video.width;
        for (unsigned x=0;x<s->video.width;++x) AV_WL16(frame->data[0]+y*frame->linesize[0]+x*2,row[x]);
#else
        memcpy(frame->data[0]+(size_t)y*frame->linesize[0],(uint8_t *)raw.buffer+(size_t)y*s->video.width*2,s->video.width*2);
#endif
    }
    av_free(raw.buffer);*got_frame=1;return packet->size;
}
const FFCodec ff_gpr_stock_decoder={
    .p.name="gpr_stock",
    CODEC_LONG_NAME("Stock GoPro RAW VC-5 framed GVID"),
    .p.type=AVMEDIA_TYPE_VIDEO,
    .p.id=AV_CODEC_ID_GPR_STOCK,
    .priv_data_size=sizeof(GPRStockContext),
    .init=stock_init,
    FF_CODEC_DECODE_CB(stock_decode),
    .p.capabilities=AV_CODEC_CAP_DR1,
};
