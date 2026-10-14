// SuperResMetal.h — MPSGraph + optional hand-rolled Metal NAFBlock super-res CNN.
//
// Drop-in replacement for SuperResCNN that uses MPSGraph (and optionally
// validated Metal kernels for the hot NAFBlocks at C=16/32/64) instead of
// CoreML's MLModel. The F backbone (variant=F or F_no_sr, width=16, depth=3)
// is hard-coded; weights are loaded from a directory of fp16 .bin blobs
// produced by extract_F_weights.py.
//
// Two head modes (selected via the `useSubpixelHead:` init flag):
//   - useSubpixelHead=YES (F variant, 2× super-res — default for backward
//     compat): final head is Conv3x3 16→16 + PixelShuffle(2) → 4 channels @
//     2× plane dims. Final Bayer adds residual to a bicubic baseline of the
//     input and outputs (2·inW × 2·inH) uint16.
//   - useSubpixelHead=NO  (F_no_sr variant, BIBO_1x clean-bayer): final head
//     is Conv3x3 16→4 (the `outro` layer) at the same plane dims as input.
//     Final Bayer adds residual to the input planes (no bicubic, no scale-up)
//     and outputs (inW × inH) uint16.
//
// Input convention (Bayer):  (inW × inH) uint16 raw mosaic
// CNN input planes:          (1, 4, inH/2, inW/2) fp16 (R, Gr, Gb, B)
// CNN output (residual)
//   - 2× mode: (1, 4, inH, inW) fp16
//   - 1× mode: (1, 4, inH/2, inW/2) fp16  (same dims as input planes)
// Final out (after baseline + scale + clamp + re-bayer)
//   - 2× mode: (2·inW × 2·inH) uint16
//   - 1× mode: (inW × inH) uint16
//
// The class allocates persistent MTLBuffers for weights and feeds + receives
// fp16 tensors from MPSGraph using NHWC layout.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SuperResMetalBackend) {
    // All ops in MPSGraph (default). Most reliable.
    SuperResMetalBackendMPSGraph = 0,
    // Hybrid: MPSGraph for intro/down/up/middle/SR head; Metal kernels for
    // NAFBlocks at C=16/32/64 (uses validated kernels from dering_proto_v2).
    SuperResMetalBackendHybrid = 1,
};

@interface SuperResMetal : NSObject

// Loads weights from `weightsDir` (a directory containing intro_weight.bin etc.
// produced by extract_F_weights.py). The (inH, inW) are *padded* CNN plane dims
// — the network's spatial input — and must be multiples of 8.
//
// 2× super-res init (backward-compatible). Uses the F variant subpixel head.
- (nullable instancetype)initWithWeightsDir:(NSString *)weightsDir
                                     device:(id<MTLDevice>)device
                                  inPlaneH:(uint32_t)inPlaneH
                                  inPlaneW:(uint32_t)inPlaneW
                                    backend:(SuperResMetalBackend)backend;

// Generalized init. `useSubpixelHead=YES` (F, 2× SR head — same behavior as
// the 4-arg init). `useSubpixelHead=NO` (F_no_sr, BIBO_1x — no bicubic, no
// PixelShuffle; head is Conv3x3 16→4 at same dims as input planes).
- (nullable instancetype)initWithWeightsDir:(NSString *)weightsDir
                                     device:(id<MTLDevice>)device
                                  inPlaneH:(uint32_t)inPlaneH
                                  inPlaneW:(uint32_t)inPlaneW
                                    backend:(SuperResMetalBackend)backend
                            useSubpixelHead:(BOOL)useSubpixelHead;

// Same signature as SuperResCNN. inBayer is (inW * inH) uint16; outBayer
// (outW * outH) uint16. The CNN runs on inH/2 × inW/2 planes and produces
// outH × outW Bayer output (typically 2× the input).
// Returns 0 on success.
- (int)runOnBayer:(const uint16_t *)inBayer
            width:(uint32_t)inW height:(uint32_t)inH
         outBayer:(uint16_t *)outBayer
         outWidth:(uint32_t)outW
        outHeight:(uint32_t)outH
       blackLevel:(uint32_t)blackLevel
       whiteLevel:(uint32_t)whiteLevel;

// Zero-copy variant: writes the final Bayer directly into `outMTLBuffer`
// (typically an IOSurface-backed MTLBuffer shared with a CVPixelBuffer that
// CIRAWFilter will consume next). `outStrideBytes` is the destination row
// stride in bytes — must be even and >= outW * 2. The output is written
// honoring that stride (CoreVideo pads bytes-per-row to 64-byte alignment
// regardless of hints, so for typical 4140-pixel rows this is 8320 vs the
// natural 8280).
//
// The buffer must be at least (outH * outStrideBytes) bytes. No CPU memcpy
// is performed — the rebayer kernel writes directly into the IOSurface
// backing memory. The CPU returns once GPU work has been committed AND
// waited on (same sync semantics as the legacy runOnBayer:).
//
// Returns 0 on success.
- (int)runOnBayer:(const uint16_t *)inBayer
            width:(uint32_t)inW height:(uint32_t)inH
     outMTLBuffer:(id<MTLBuffer>)outMTLBuffer
   outStrideBytes:(size_t)outStrideBytes
         outWidth:(uint32_t)outW
        outHeight:(uint32_t)outH
       blackLevel:(uint32_t)blackLevel
       whiteLevel:(uint32_t)whiteLevel;

@end

NS_ASSUME_NONNULL_END
