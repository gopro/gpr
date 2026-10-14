// GPRFileReader.m — read encoded .gpr files.
//
// We use mmap'd-backed NSData so the OS handles paging; for ~3.5 MB files
// on NVMe this resolves to ~1-2 ms of effective wall-clock per frame. The
// file is not modified, so NSDataReadingMappedAlways is the right policy.

#import "GPRFileReader.h"

// Field offsets in the FUSED_HEADER (see fused_encode.h). Keep these in sync.
//   magic=0 version=4 width=8 height=12 pixel_format=16 quality=20 is_rggb=24
//   log_bits=28 prescale=32 multi_level=36 num_bands=40 decimate=44
static uint32_t le32_at(const uint8_t *p, size_t off) {
    return  (uint32_t)p[off+0]
         | ((uint32_t)p[off+1] << 8)
         | ((uint32_t)p[off+2] << 16)
         | ((uint32_t)p[off+3] << 24);
}

extern int fast_gpr_extract_vc5(const uint8_t *gpr_data, size_t gpr_size,
                                size_t *vc5_offset, size_t *vc5_size,
                                int *image_width, int *image_height);

static BOOL parse_header_bytes(const uint8_t *bytes, size_t len, GPRFileInfo *info) {
    if (!bytes || len < 48) return NO;
    uint32_t magic = le32_at(bytes, 0);
    if (magic == 0x44535546u) {
        info->encWidth    = le32_at(bytes,  8);
        info->encHeight   = le32_at(bytes, 12);
        info->pixelFormat = le32_at(bytes, 16);
        uint32_t dec      = le32_at(bytes, 44);
        info->decimate    = (dec < 2) ? 1 : dec;
        info->decWidth    = info->encWidth  / info->decimate;
        info->decHeight   = info->encHeight / info->decimate;
        info->containerGPR = 0;
        return YES;
    }

    size_t vc5Offset = 0, vc5Size = 0;
    int imageWidth = 0, imageHeight = 0;
    if (fast_gpr_extract_vc5(bytes, len, &vc5Offset, &vc5Size, &imageWidth, &imageHeight) == 0 &&
        imageWidth > 0 && imageHeight > 0) {
        info->encWidth = (uint32_t)imageWidth;
        info->encHeight = (uint32_t)imageHeight;
        info->pixelFormat = 1; // TIFF/GPR fallback currently assumes RGGB14.
        info->decimate = 1;
        info->decWidth = info->encWidth;
        info->decHeight = info->encHeight;
        info->containerGPR = 1;
        return YES;
    }

    fprintf(stderr, "GPRFileReader: bad magic 0x%08x (expected 'FUSD' or TIFF/GPR)\n", magic);
    return NO;
}

@implementation GPRFileReader

+ (nullable NSData *)readEncodedFromPath:(NSString *)path info:(GPRFileInfo *)info {
    NSError *err = nil;
    NSData *data = [NSData dataWithContentsOfFile:path
                                          options:NSDataReadingMappedAlways
                                            error:&err];
    if (!data) {
        fprintf(stderr, "GPRFileReader: open %s failed: %s\n",
                [path UTF8String],
                err ? [[err localizedDescription] UTF8String] : "?");
        return nil;
    }
    if (!parse_header_bytes(data.bytes, data.length, info)) {
        fprintf(stderr, "GPRFileReader: bad GPR header in %s\n", [path UTF8String]);
        return nil;
    }
    return data;
}

+ (BOOL)readHeaderFromPath:(NSString *)path info:(GPRFileInfo *)info {
    NSFileHandle *fh = [NSFileHandle fileHandleForReadingAtPath:path];
    if (!fh) return NO;
    NSData *hdr = [fh readDataToEndOfFile];
    [fh closeFile];
    return parse_header_bytes(hdr.bytes, hdr.length, info);
}

@end
