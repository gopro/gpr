// CIRawDemosaic.h — Apple-grade demosaic via CIRAWFilter.
//
// Alternative to Demosaic.{h,m}. Same input contract: take a uint16 Bayer
// plane and render the demosaiced RGB into a CVPixelBuffer.
//
// Internally synthesizes a minimal in-memory DNG (via CIDNGBuilder), feeds
// it to CIRAWFilter via filterWithImageData:identifierHint:options:, and
// renders with a Metal-backed CIContext.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#import "DNGReader.h"

NS_ASSUME_NONNULL_BEGIN

@interface CIRawDemosaic : NSObject

// Sensor-side dims (full Bayer mosaic) + DNGInfo (color matrices, WB, etc.).
// outWidth/outHeight: the target render dims that the CVPixelBuffer the
// caller will pass to -encode: is sized to.
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            sensorWidth:(uint32_t)sensorW
                           sensorHeight:(uint32_t)sensorH
                              outWidth:(uint32_t)outW
                             outHeight:(uint32_t)outH
                                   info:(const DNGInfo *)info
                         templateDngPath:(nullable NSString *)templateDngPath
                                lookMode:(nullable NSString *)lookMode;

// Encode a demosaic pass into `pb`. Synchronous: returns when the GPU work
// has been committed (matches Demosaic's pattern, where the pipeline blocks
// before handing the pb to ProRes). `cb` is unused; pass nil.
- (void)encode:(nullable id<MTLCommandBuffer>)cb
         bayer:(const uint16_t *)bayer
         width:(uint32_t)w
        height:(uint32_t)h
outPixelBuffer:(CVPixelBufferRef)pb;

// Zero-copy entry point: the caller has already filled `bayerPB` with the
// 14Bayer<CFA> Bayer plane (typically written into the IOSurface backing
// memory by an upstream Metal kernel). This skips the internal pool +
// memcpy entirely and hands `bayerPB` straight to CIRAWFilter.
//
// The CVPixelBuffer's format must be one of the kCVPixelFormatType_14Bayer_*
// CFA constants matching the DNGInfo->cfaPattern passed at init. Its dims
// must match the configured sensor dims.
//
// The pixel buffer must not be CPU-locked while this is called: CIRAWFilter
// needs the IOSurface in a consumable state.
- (void)encodeFromBayerPixelBuffer:(CVPixelBufferRef)bayerPB
                    outPixelBuffer:(CVPixelBufferRef)pb;

@end

NS_ASSUME_NONNULL_END
