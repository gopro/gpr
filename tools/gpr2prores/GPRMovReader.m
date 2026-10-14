// MOV reader adapter: shared portable payload parsing plus optional EXIF sidecar.
#import "GPRMovReader.h"
#import "DNGExif.h"
#include "../gvid_mov/include/gvid_mov.h"
#include <libavutil/error.h>
#include <string.h>

@implementation GPRMovReader {
    GVIDMov_Reader *_reader;
    NSData *_exifSidecar;
    size_t _exifSidecarOff;
    BOOL _readFailed;
}

- (nullable instancetype)initWithPath:(NSString *)path info:(GPRMovInfo *)info {
    self = [super init];
    if (!self) return nil;
    _reader = gvid_mov_reader_open(path.fileSystemRepresentation);
    if (!_reader) return nil;
    NSString *exifPath = [path stringByAppendingPathExtension:@"exif"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:exifPath]) {
        _exifSidecar = [NSData dataWithContentsOfFile:exifPath
                                            options:NSDataReadingMappedAlways error:nil];
        if (!_exifSidecar || _exifSidecar.length < 4 ||
            memcmp(_exifSidecar.bytes, "GEX0", 4) != 0) {
            fprintf(stderr, "GPRMovReader: invalid EXIF sidecar\n");
            return nil;
        }
        _exifSidecarOff = 4;
    }
    if (info) {
        memset(info, 0, sizeof(*info));
        int width = 0, height = 0, num = 0, den = 1, audio = 0, timecode = 0;
        gvid_mov_reader_get_video_info(_reader, &width, &height, &num, &den,
                                      &info->numFrames);
        info->width = width;
        info->height = height;
        info->fps = den > 0 && num >= den ? num / den : 24;
        gvid_mov_reader_get_aux_info(_reader, &audio, &timecode,
                                    info->timecodeStart, sizeof(info->timecodeStart));
        info->hasAudio = audio != 0;
        info->hasTimecode = timecode != 0;
        info->hasExif = _exifSidecar != nil;
    }
    return self;
}

- (BOOL)readFailed { return _readFailed; }

- (void)dealloc { [self close]; }

- (nullable DNGExifInfo *)nextExifFromSidecar {
    if (!_exifSidecar) return nil;
    if (_exifSidecarOff > _exifSidecar.length ||
        _exifSidecar.length - _exifSidecarOff < 4) {
        _readFailed = YES;
        return nil;
    }
    const uint8_t *p = _exifSidecar.bytes;
    uint32_t length = 0;
    memcpy(&length, p + _exifSidecarOff, sizeof(length));
    _exifSidecarOff += sizeof(length);
    if (length > _exifSidecar.length - _exifSidecarOff) {
        _readFailed = YES;
        return nil;
    }
    NSData *blob = [NSData dataWithBytes:p + _exifSidecarOff length:length];
    _exifSidecarOff += length;
    return [DNGExif deserialize:blob];
}

- (nullable NSData *)nextFrame { return [self nextFrameWithExif:NULL]; }

- (nullable NSData *)nextFrameWithExif:(DNGExifInfo * _Nullable * _Nullable)outExif {
    if (!_reader || _readFailed) return nil;
    if (outExif) *outExif = nil;
    const uint8_t *bytes = NULL;
    size_t length = 0;
    int rc = gvid_mov_reader_next_frame(_reader, &bytes, &length, NULL);
    if (rc < 0) {
        _readFailed = rc != AVERROR_EOF;
        return nil;
    }
    DNGExifInfo *exif = [self nextExifFromSidecar];
    if (outExif) *outExif = exif;
    if (_readFailed) return nil;
    return [NSData dataWithBytes:bytes length:length];
}

- (void)close {
    if (_reader) {
        gvid_mov_reader_close(_reader);
        _reader = NULL;
    }
}
@end
