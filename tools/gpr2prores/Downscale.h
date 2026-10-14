// Downscale.h — bilinear BGRA8→BGRA8 GPU downscale.
//
// Use this after the Metal-bilinear demosaic when the output ProRes res is
// smaller than the bayer res. Construction-time dims are fixed; a single
// command-buffer encode reads from an input CVPixelBuffer and writes to an
// output CVPixelBuffer.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>

NS_ASSUME_NONNULL_BEGIN

@interface Downscale : NSObject

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                inWidth:(uint32_t)inW
                               inHeight:(uint32_t)inH
                               outWidth:(uint32_t)outW
                              outHeight:(uint32_t)outH;

// Encode a downscale pass into `cb`. Both buffers must be IOSurface-backed
// BGRA8 CVPixelBuffers. `inPB` is the post-demosaic full-res image; `outPB`
// is the ProRes-bound smaller buffer.
- (void)encode:(id<MTLCommandBuffer>)cb
        inPixelBuffer:(CVPixelBufferRef)inPB
       outPixelBuffer:(CVPixelBufferRef)outPB;

@end

NS_ASSUME_NONNULL_END
