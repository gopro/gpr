// GPRFileReader.h — read encoded .gpr files for the playback pipeline.
//
// A .gpr file holds the encoded fused-codec bitstream emitted by
// gpr_encode_fused_frame (with the FUSED_HEADER + per-band data). The
// playback pipeline reads these directly and runs only the decode stage
// (no encode), so on-disk -> NEON-decode -> ~50 ms / frame on M3 Max.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef struct {
    uint32_t encWidth;       // FUSED_HEADER.width  (full bayer, e.g. 8280)
    uint32_t encHeight;      // FUSED_HEADER.height (full bayer, e.g. 5520)
    uint32_t decimate;       // 1 = no decimation, 2 = 2x decimated bands
    uint32_t pixelFormat;    // FUSED_HEADER.pixel_format
    uint32_t decWidth;       // encWidth  / max(decimate,1)
    uint32_t decHeight;      // encHeight / max(decimate,1)
    uint32_t containerGPR;    // 0 = direct FUSD payload, 1 = TIFF/GPR container
} GPRFileInfo;

@interface GPRFileReader : NSObject

// Read a .gpr file's encoded bytes into a freshly-allocated buffer.
// Returns nil on failure. Caller owns the returned NSData.
// `info` is filled in on success from the FUSED_HEADER bytes.
+ (nullable NSData *)readEncodedFromPath:(NSString *)path info:(GPRFileInfo *)info;

// Parse just the FUSED_HEADER fields without holding the file open.
// Returns YES on success.
+ (BOOL)readHeaderFromPath:(NSString *)path info:(GPRFileInfo *)info;

@end

NS_ASSUME_NONNULL_END
