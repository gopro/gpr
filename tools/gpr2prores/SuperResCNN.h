// SuperResCNN.h — CoreML super-res CNN.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

@interface SuperResCNN : NSObject

- (nullable instancetype)initWithMLPackagePath:(NSString *)path
                                         device:(id<MTLDevice>)device;

// Bayer in (width × height uint16) → super-res Bayer out (outW × outH uint16).
// The CNN takes 4-plane fp16 input at (1, 4, h/2, w/2) and produces a 2× output
// residual that we combine with bicubic-baseline + residual_scale * residual.
// Returns 0 on success.
- (int)runOnBayer:(const uint16_t *)inBayer
            width:(uint32_t)inW height:(uint32_t)inH
         outBayer:(uint16_t *)outBayer
         outWidth:(uint32_t)outW
        outHeight:(uint32_t)outH
       blackLevel:(uint32_t)blackLevel
       whiteLevel:(uint32_t)whiteLevel;

@end

NS_ASSUME_NONNULL_END
