// CIDNGBuilder.h — Build a minimal in-memory DNG NSData blob from a raw
// uint16 Bayer plane plus pre-configured per-session metadata.
//
// Purpose: feed CIRAWFilter via filterWithImageData:identifierHint:options:
// without writing a temp file. The "static" tag block (color matrices, WB,
// black/white, CFA pattern, camera model) is computed once at init time and
// stored as a template NSMutableData; per-frame we only swap the strip data.

#import <Foundation/Foundation.h>
#import "DNGReader.h"  // for DNGInfo

NS_ASSUME_NONNULL_BEGIN

@interface CIDNGBuilder : NSObject

// Init with sensor metadata + bayer dimensions. width/height are the Bayer
// dims of the raw plane (e.g. 8280×5520 for Z8). If `templateDngPath` is
// non-nil, the ColorMatrix1, ColorMatrix2, ForwardMatrix1/2, and
// CalibrationIlluminant1/2 tags are sourced from that DNG verbatim
// (recommended — keeps Apple's color science faithful to the camera profile
// embedded in the original file).
- (instancetype)initWithInfo:(const DNGInfo *)info
                       width:(uint32_t)width
                      height:(uint32_t)height
             templateDngPath:(nullable NSString *)templateDngPath;

// Build a fresh NSData containing a valid DNG file with strip data sourced
// from `bayerBytes` (raw uint16 LE, length must equal width*height*2).
// Returns nil if length mismatches.
- (nullable NSData *)dngFromBayerBytes:(const void *)bayerBytes
                                length:(size_t)length;

// The bayer dimensions captured at init.
@property (nonatomic, readonly) uint32_t width;
@property (nonatomic, readonly) uint32_t height;

@end

NS_ASSUME_NONNULL_END
