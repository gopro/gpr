// Demosaic.h — bilinear Bayer→RGB on Metal.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>

NS_ASSUME_NONNULL_BEGIN

@interface Demosaic : NSObject

// Width/height = Bayer dims at construction time (full Bayer mosaic).
// Output BGRA8 texture has the same dims.
- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                  width:(uint32_t)w
                                 height:(uint32_t)h
                             cfaPattern:(uint32_t)cfaPattern
                             blackLevel:(uint32_t)blackLevel
                             whiteLevel:(uint32_t)whiteLevel
                                    wbR:(float)wbR
                                    wbG:(float)wbG
                                    wbB:(float)wbB
                                rgbCam:(const float (*)[3])rgbCam;


// Encode demosaic into `cb`. `bayer` is the input mosaic; we upload it to a
// Metal buffer each frame (no copy avoided yet — phase 1).
// Output goes into the IOSurface-backed CVPixelBuffer.
- (void)encode:(id<MTLCommandBuffer>)cb
         bayer:(const uint16_t *)bayer
         width:(uint32_t)w
        height:(uint32_t)h
outPixelBuffer:(CVPixelBufferRef)pb;

@end

NS_ASSUME_NONNULL_END
