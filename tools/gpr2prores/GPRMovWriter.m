// GPRMovWriter.m — libavformat-backed MOV writer for GPR-frame streams.
//
// Why libavformat: AVFoundation doesn't expose arbitrary codec_tag injection
// for custom codecs without re-implementing a low-level muxer. libavformat
// lets us set codec_tag = MKTAG('G','P','R','1') and codec_id = AV_CODEC_ID_NONE
// directly on the stream, then write packets via av_interleaved_write_frame.
//
// New in this version:
//   - Optional timecode (TMCD) stream — SMPTE start TC + drop-frame flag.
//   - Optional audio track — PCM s16le passthrough from a WAV file.
//   - Optional per-frame EXIF — sidecar GEXF stream with one packet per frame.

#import "GPRMovWriter.h"
#import "DNGExif.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/timestamp.h>
#include <libavutil/intreadwrite.h>
#include <math.h>
#include <limits.h>

#define GPR_FOURCC  MKTAG('G','P','R','1')
#define TMCD_FOURCC MKTAG('t','m','c','d')

@implementation GPRMovWriter {
    AVFormatContext *_fmtCtx;
    AVStream *_vstream;     // GPR1 video stream
    AVStream *_astream;     // optional audio
    AVStream *_tstream;     // optional timecode (tmcd)
    int64_t _pts;
    int _fps;
    BOOL _wroteHeader;

    // EXIF sidecar file (e.g. "clip.mov.exif") — appended per-frame as
    // length-prefixed GEXF blobs. The MOV muxer doesn't reliably preserve
    // codec_tag on data streams, so a sidecar is the simplest robust path.
    // Layout: "GEX0" magic (4) + repeat { uint32_le len; bytes }.
    FILE *_exifFp;
    NSString *_exifPath;

    // Audio: PCM s16 samples staged here, written after each video frame so
    // the muxer can interleave properly. For simple passthrough we load
    // the entire decoded PCM up-front and pace it 1/fps of video.
    NSData *_pcmData;       // s16 interleaved
    int _audioSampleRate;
    int _audioChannels;
    int64_t _audioSamplesWritten;

    // Timecode start, encoded as a 32-bit frame number (BE) per the TMCD spec.
    BOOL _hasTC;
    uint32_t _tcFrames;
    BOOL _tcDropFrame;
}

- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                  fps:(int)fps
{
    self = [super init];
    if (!self) return nil;
    if (!path.length || !width || !height || width > INT_MAX || height > INT_MAX || fps <= 0)
        return nil;
    _fps = fps;
    _pts = 0;
    _wroteHeader = NO;

    const char *cpath = [path UTF8String];
    if (avformat_alloc_output_context2(&_fmtCtx, NULL, "mov", cpath) < 0 || !_fmtCtx) {
        fprintf(stderr, "GPRMovWriter: alloc_output_context2 failed for %s\n", cpath);
        return nil;
    }

    _vstream = avformat_new_stream(_fmtCtx, NULL);
    if (!_vstream) {
        fprintf(stderr, "GPRMovWriter: new_stream(video) failed\n");
        avformat_free_context(_fmtCtx); _fmtCtx = NULL;
        return nil;
    }
    _vstream->id = 0;
    _vstream->time_base = (AVRational){ 1, fps };
    _vstream->avg_frame_rate = (AVRational){ fps, 1 };

    AVCodecParameters *par = _vstream->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id   = AV_CODEC_ID_NONE;   // custom codec
    par->codec_tag  = GPR_FOURCC;
    par->width      = (int)width;
    par->height     = (int)height;
    par->format     = AV_PIX_FMT_NONE;
    par->bit_rate   = 0;

    // Sidecar path (only opened lazily if EXIF is ever appended).
    _exifPath = [path stringByAppendingPathExtension:@"exif"];
    return self;
}

// Open the file + write_header. Done lazily on first append so optional
// streams (timecode/audio/exif) can be added beforehand.
- (BOOL)openIfNeeded {
    if (_wroteHeader) return YES;
    if (!_fmtCtx) return NO;
    const char *cpath = _fmtCtx->url ? _fmtCtx->url : "<unknown>";
    if (!(_fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&_fmtCtx->pb, cpath, AVIO_FLAG_WRITE) < 0) {
            fprintf(stderr, "GPRMovWriter: avio_open(%s) failed\n", cpath);
            return NO;
        }
    }
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([fm fileExistsAtPath:_exifPath] && ![fm removeItemAtPath:_exifPath error:nil]) {
        fprintf(stderr, "GPRMovWriter: cannot remove stale EXIF sidecar\n");
        return NO;
    }
    AVDictionary *opts = NULL;
    // Use frag_keyframe so we can write to a pipe / streaming target. Each
    // GPR frame is a keyframe, so this is safe.
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov", 0);
    int rc = avformat_write_header(_fmtCtx, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        char errbuf[256] = {0};
        av_strerror(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "GPRMovWriter: write_header failed: %s\n", errbuf);
        if (!(_fmtCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&_fmtCtx->pb);
        return NO;
    }
    _wroteHeader = YES;
    int nVideo = _vstream ? 1 : 0;
    int nAudio = _astream ? 1 : 0;
    int nTmcd = _tstream ? 1 : 0;
    fprintf(stderr, "GPRMovWriter: %s opened, tracks: video=%d audio=%d tmcd=%d (%dx%d@%dfps)\n",
            cpath, nVideo, nAudio, nTmcd,
            _vstream->codecpar->width, _vstream->codecpar->height, _fps);
    return YES;
}

- (void)dealloc {
    if (_exifFp) { fclose(_exifFp); _exifFp = NULL; }
    if (_fmtCtx) {
        if (_wroteHeader) av_write_trailer(_fmtCtx);
        if (!(_fmtCtx->oformat->flags & AVFMT_NOFILE)) avio_closep(&_fmtCtx->pb);
        avformat_free_context(_fmtCtx);
        _fmtCtx = NULL;
    }
}

// --- Timecode --------------------------------------------------------------

static uint32_t tc_to_frames(int hh, int mm, int ss, int ff, int fps, BOOL dropFrame) {
    // Non-drop: linear. Drop-frame (NTSC 29.97/59.94): per SMPTE 12M.
    if (!dropFrame) {
        return (uint32_t)(((hh * 3600 + mm * 60 + ss) * fps) + ff);
    }
    // SMPTE drop-frame: drop 2 frames at the start of every minute except every
    // 10th. (For 60-frame rates double everything.) Implementation per SMPTE 12M.
    int dropFramesPerMin = (fps == 60 || fps == 59) ? 4 : 2;
    int framesPerMin = fps * 60 - dropFramesPerMin;
    int framesPer10Min = framesPerMin * 10 + dropFramesPerMin;
    int totalMins = hh * 60 + mm;
    int n = totalMins / 10;
    int rem = totalMins - n * 10;
    int frames = n * framesPer10Min
               + (rem ? (framesPerMin + (rem - 1) * framesPerMin) : 0)
               + ss * fps + ff;
    return (uint32_t)frames;
}

- (BOOL)addTimecodeStart:(NSString *)tcStart dropFrame:(BOOL)dropFrame {
    if (_wroteHeader || _tstream) return NO;
    int hh=0, mm=0, ss=0, ff=0;
    // Accept "HH:MM:SS:FF" and "HH:MM:SS;FF" (latter signals drop-frame).
    if (sscanf([tcStart UTF8String], "%d:%d:%d:%d", &hh, &mm, &ss, &ff) != 4) {
        if (sscanf([tcStart UTF8String], "%d:%d:%d;%d", &hh, &mm, &ss, &ff) == 4) {
            dropFrame = YES;
        } else {
            fprintf(stderr, "GPRMovWriter: bad timecode '%s' (want HH:MM:SS:FF)\n", [tcStart UTF8String]);
            return NO;
        }
    }
    _tstream = avformat_new_stream(_fmtCtx, NULL);
    if (!_tstream) return NO;
    AVCodecParameters *p = _tstream->codecpar;
    p->codec_type = AVMEDIA_TYPE_DATA;
    p->codec_id   = AV_CODEC_ID_NONE;
    p->codec_tag  = TMCD_FOURCC;
    _tstream->time_base = (AVRational){ 1, _fps };
    _tstream->avg_frame_rate = (AVRational){ _fps, 1 };

    // Encode start TC as a tag on the stream metadata (libavformat MOV muxer
    // recognizes "timecode" key and writes a proper TMCD atom).
    char buf[32]; snprintf(buf, sizeof(buf), "%02d:%02d:%02d%c%02d", hh, mm, ss, dropFrame ? ';' : ':', ff);
    av_dict_set(&_tstream->metadata, "timecode", buf, 0);

    _hasTC = YES;
    _tcFrames = tc_to_frames(hh, mm, ss, ff, _fps, dropFrame);
    _tcDropFrame = dropFrame;
    fprintf(stderr, "GPRMovWriter: timecode start %s%s (frames=%u)\n",
            buf, dropFrame ? " [DF]" : "", _tcFrames);
    return YES;
}

// --- Audio: WAV passthrough -----------------------------------------------

// Minimal WAV parser. Accepts PCM s16/s24 mono/stereo. Returns NSData of
// s16-interleaved PCM, with sample rate and channels filled in. Returns nil
// on parse failure.
static NSData *loadWAV(NSString *path, int *outSR, int *outCH) {
    NSData *blob = [NSData dataWithContentsOfFile:path];
    if (!blob || blob.length < 44) {
        fprintf(stderr, "GPRMovWriter: cannot open WAV %s\n", [path UTF8String]);
        return nil;
    }
    const uint8_t *p = blob.bytes;
    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "GPRMovWriter: not a WAV file: %s\n", [path UTF8String]);
        return nil;
    }
    size_t riffSize = (size_t)AV_RL32(p + 4) + 8;
    if (riffSize < 12 || riffSize > blob.length) return nil;
    size_t off = 12;
    int sr = 0, ch = 0, bps = 0; uint16_t fmtCode = 0;
    const uint8_t *dataPtr = NULL; size_t dataLen = 0;
    while (off + 8 <= riffSize) {
        uint32_t id4 = AV_RL32(p + off);
        uint32_t sz  = AV_RL32(p + off + 4);
        const uint8_t *body = p + off + 8;
        if ((size_t)sz > riffSize - off - 8) return nil;
        if (id4 == AV_RL32("fmt ") && sz >= 16) {
            fmtCode = AV_RL16(body + 0);
            ch  = AV_RL16(body + 2);
            sr  = AV_RL32(body + 4);
            bps = AV_RL16(body + 14);
        } else if (id4 == AV_RL32("data")) {
            dataPtr = body; dataLen = sz;
            break;
        }
        off += 8 + (((size_t)sz + 1) & ~(size_t)1); // pad to even
    }
    if (!dataPtr || sr <= 0 || sr > 768000 || ch <= 0 || ch > 2 ||
        !((fmtCode == 1 && (bps == 16 || bps == 24)) || (fmtCode == 3 && bps == 32)) ||
        dataLen % (ch * (bps / 8)) != 0) {
        fprintf(stderr, "GPRMovWriter: unsupported WAV (sr=%d ch=%d bps=%d fmt=%u)\n", sr, ch, bps, fmtCode);
        return nil;
    }

    // Convert to s16 interleaved if needed.
    size_t nSamples = dataLen / (ch * (bps / 8));
    NSMutableData *out = [NSMutableData dataWithLength:nSamples * ch * 2];
    int16_t *o = out.mutableBytes;
    if (bps == 16) {
        memcpy(o, dataPtr, nSamples * ch * 2);
    } else if (bps == 24) {
        const uint8_t *s = dataPtr;
        for (size_t i = 0; i < nSamples * ch; i++) {
            int32_t v = (int32_t)((s[3*i] | (s[3*i+1] << 8) | (s[3*i+2] << 16))
                                  | ((s[3*i+2] & 0x80) ? 0xFF000000u : 0));
            // 24-bit → 16-bit by truncation (drop low 8 bits).
            o[i] = (int16_t)(v >> 8);
        }
    } else if (bps == 32) {
        // f32 (assume IEEE float) → s16
        for (size_t i = 0; i < nSamples * ch; i++) {
            uint32_t bits = AV_RL32(dataPtr + i * 4);
            float v;
            memcpy(&v, &bits, sizeof(v));
            if (!isfinite(v)) return nil;
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            o[i] = (int16_t)(v * 32767.0f);
        }
    }
    *outSR = sr; *outCH = ch;
    return out;
}

- (BOOL)addAudioFromWAV:(NSString *)wavPath {
    if (_wroteHeader || _astream) return NO;
    int sr = 0, ch = 0;
    NSData *pcm = loadWAV(wavPath, &sr, &ch);
    if (!pcm) return NO;

    _astream = avformat_new_stream(_fmtCtx, NULL);
    if (!_astream) return NO;
    AVCodecParameters *p = _astream->codecpar;
    p->codec_type    = AVMEDIA_TYPE_AUDIO;
    p->codec_id      = AV_CODEC_ID_PCM_S16LE;
    // MOV uses 'sowt' for s16 little-endian PCM.
    p->codec_tag     = MKTAG('s','o','w','t');
    p->sample_rate   = sr;
    p->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC;
    p->ch_layout.nb_channels = ch;
    p->format        = AV_SAMPLE_FMT_S16;
    p->bits_per_coded_sample = 16;
    p->block_align   = ch * 2;
    p->bit_rate      = sr * ch * 16;
    _astream->time_base = (AVRational){ 1, sr };

    _pcmData = pcm;
    _audioSampleRate = sr;
    _audioChannels = ch;
    _audioSamplesWritten = 0;
    fprintf(stderr, "GPRMovWriter: audio %d Hz / %d ch / s16 / %.2f s\n",
            sr, ch, (double)(pcm.length / (ch * 2)) / sr);
    return YES;
}

// --- EXIF sidecar ----------------------------------------------------------
//
// Per-frame EXIF is stored in a sibling .exif file rather than as a track in
// the MOV. Rationale: libavformat's mov muxer doesn't reliably preserve a
// custom codec_tag on data streams — it rewrites the sample entry's fourcc
// to "stts" or similar, making the data un-readable by our codec_tag-keyed
// reader. A sidecar is the simplest, most robust approach. NLEs ignore it
// (it has a non-standard extension) and our reader pairs it by basename.
//
// Format: 4-byte magic "GEX0" + repeated { uint32_le len; bytes }.

- (BOOL)openExifSidecarIfNeeded {
    if (_exifFp) return YES;
    _exifFp = fopen([_exifPath UTF8String], "wb");
    if (!_exifFp) {
        fprintf(stderr, "GPRMovWriter: cannot open EXIF sidecar %s (%s)\n",
                [_exifPath UTF8String], strerror(errno));
        return NO;
    }
    const char magic[4] = {'G','E','X','0'};
    if (fwrite(magic, 1, 4, _exifFp) != 4) return NO;
    // EXIF can first appear after frames without metadata.
    const uint32_t missing = 0;
    for (int64_t frame = 0; frame < _pts; frame++) {
        if (fwrite(&missing, sizeof(missing), 1, _exifFp) != 1) return NO;
    }
    return YES;
}

- (int)writeExifBlob:(NSData *)blob {
    if (blob.length > UINT32_MAX) return -1;
    if (![self openExifSidecarIfNeeded]) return -1;
    uint32_t L = (uint32_t)blob.length;
    if (fwrite(&L, 4, 1, _exifFp) != 1) return -1;
    if (blob.length > 0 && fwrite(blob.bytes, 1, blob.length, _exifFp) != blob.length) return -1;
    return 0;
}

// --- packet append ---------------------------------------------------------

- (int)writeVideoPacket:(const uint8_t *)bytes length:(size_t)length {
    if (!bytes || !length || length > INT_MAX) return -1;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;
    if (av_new_packet(pkt, (int)length) < 0) { av_packet_free(&pkt); return -1; }
    memcpy(pkt->data, bytes, length);
    pkt->stream_index = _vstream->index;
    pkt->pts = _pts;
    pkt->dts = _pts;
    pkt->duration = 1;
    pkt->flags |= AV_PKT_FLAG_KEY;
    int rc = av_interleaved_write_frame(_fmtCtx, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        char errbuf[256] = {0};
        av_strerror(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "GPRMovWriter: write_frame(video) failed: %s\n", errbuf);
        return -1;
    }
    return 0;
}

- (int)writeTimecodePacket {
    if (!_tstream) return 0;
    // TMCD payload is a single 32-bit BE frame number per frame, but mp4/mov
    // muxer derives this from the 'timecode' metadata key + duration; we just
    // need one zero-length packet at PTS 0 to anchor the track.
    if (_pts != 0) return 0;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;
    if (av_new_packet(pkt, 4) < 0) { av_packet_free(&pkt); return -1; }
    uint32_t be = av_bswap32(_tcFrames);
    memcpy(pkt->data, &be, 4);
    pkt->stream_index = _tstream->index;
    pkt->pts = 0;
    pkt->dts = 0;
    pkt->duration = 1;
    pkt->flags |= AV_PKT_FLAG_KEY;
    int rc = av_interleaved_write_frame(_fmtCtx, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        char errbuf[256] = {0};
        av_strerror(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "GPRMovWriter: write_frame(tmcd) failed: %s\n", errbuf);
        return -1;
    }
    return 0;
}

- (int)writeAudioForFrame {
    if (!_astream || !_pcmData) return 0;
    // Pace audio at sr/fps samples per video frame.
    int64_t samplesPerFrame = (int64_t)_audioSampleRate / _fps;
    int64_t remainder = (int64_t)_audioSampleRate - samplesPerFrame * _fps;
    // Distribute remainder over frames.
    if ((_pts % _fps) < remainder) samplesPerFrame++;

    int64_t totalSamples = (int64_t)_pcmData.length / (_audioChannels * 2);
    if (_audioSamplesWritten >= totalSamples) return 0;
    int64_t take = MIN(samplesPerFrame, totalSamples - _audioSamplesWritten);
    if (take <= 0) return 0;

    size_t byteOff = (size_t)_audioSamplesWritten * _audioChannels * 2;
    size_t byteLen = (size_t)take * _audioChannels * 2;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return -1;
    if (av_new_packet(pkt, (int)byteLen) < 0) { av_packet_free(&pkt); return -1; }
    memcpy(pkt->data, (const uint8_t *)_pcmData.bytes + byteOff, byteLen);
    pkt->stream_index = _astream->index;
    pkt->pts = _audioSamplesWritten;
    pkt->dts = _audioSamplesWritten;
    pkt->duration = take;
    pkt->flags |= AV_PKT_FLAG_KEY;
    int rc = av_interleaved_write_frame(_fmtCtx, pkt);
    av_packet_free(&pkt);
    _audioSamplesWritten += take;
    if (rc < 0) {
        char errbuf[256] = {0};
        av_strerror(rc, errbuf, sizeof(errbuf));
        fprintf(stderr, "GPRMovWriter: write_frame(audio) failed: %s\n", errbuf);
        return -1;
    }
    return 0;
}

- (int)appendEncodedBytes:(const uint8_t *)bytes length:(size_t)length {
    return [self appendEncodedBytes:bytes length:length exif:nil];
}

- (int)appendEncodedBytes:(const uint8_t *)bytes
                   length:(size_t)length
                     exif:(nullable DNGExifInfo *)exif
{
    if (!_fmtCtx || !_vstream) return -1;
    if (![self openIfNeeded]) return -1;

    int rc = [self writeVideoPacket:bytes length:length];
    if (rc != 0) return rc;

    if (_tstream && [self writeTimecodePacket] != 0) return -1;
    if (_astream && [self writeAudioForFrame] != 0) return -1;
    if (exif || _exifFp) {
        NSData *blob = exif ? [DNGExif serialize:exif] : [NSData data];
        if (!blob || [self writeExifBlob:blob] != 0) return -1;
    }

    _pts++;
    return 0;
}

- (int)finish {
    if (!_fmtCtx) return 0;
    int status = 0;
    // Flush any remaining audio bytes.
    if (_astream && _pcmData) {
        int64_t totalSamples = (int64_t)_pcmData.length / (_audioChannels * 2);
        while (_audioSamplesWritten < totalSamples) {
            if ([self writeAudioForFrame] != 0) { status = -1; break; }
        }
    }
    if (_wroteHeader) {
        if (av_write_trailer(_fmtCtx) < 0) status = -1;
        _wroteHeader = NO;
    }
    if (!(_fmtCtx->oformat->flags & AVFMT_NOFILE) && avio_closep(&_fmtCtx->pb) < 0) status = -1;
    avformat_free_context(_fmtCtx);
    _fmtCtx = NULL;
    if (_exifFp) { if (fclose(_exifFp) != 0) status = -1; _exifFp = NULL; }
    return status;
}

@end
