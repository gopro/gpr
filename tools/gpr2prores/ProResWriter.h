// ProResWriter.h — VTCompressionSession + AVAssetWriter ProRes writer.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>

NS_ASSUME_NONNULL_BEGIN

@interface ProResWriter : NSObject

- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                  fps:(int)fps
                               device:(id<MTLDevice>)device;

// Get a CVPixelBuffer that the demosaic kernel can render INTO via its
// IOSurface-backed Metal texture. Returns the same buffer per call (the pool
// rotates so the previous frame's buffer is recycled once encoded). nil on
// failure.
- (nullable CVPixelBufferRef)pixelBuffer CF_RETURNS_NOT_RETAINED;

// Submit the (Metal-rendered) pixel buffer to the asset writer.
- (int)appendPixelBuffer:(CVPixelBufferRef)pb frameIndex:(int)idx;

- (int)finish;

@end

NS_ASSUME_NONNULL_END
