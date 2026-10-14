// GPRPipeline.h — the top-level pipeline class.
//
// Two input modes:
//   - DNG mode: encodes + decodes through the fused codec per frame
//     (acquisition-time / single-pass benchmarking).
//   - GPR mode: reads pre-encoded .gpr files and runs decode only
//     (playback workflow). Color/black/white/wb metadata comes from a
//     companion DNG passed as metaDngPath.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface GPRPipeline : NSObject

// firstFrame: path to the first input frame (.dng or .gpr).
// metaDngPath: when firstFrame is .gpr, the DNG to pull color/black/white/wb
//   metadata from. When firstFrame is .dng this may be nil.
// cnnBackend: "coreml", "mpsgraph", or "metal"; must be compiled in.
// nil selects CoreML when built, otherwise Metal. With no backends, noCNN
// must be YES. An unavailable backend with noCNN=NO fails initialization.
// When "mpsgraph"/"metal", ckptPath is interpreted as a directory of fp16 weight
// blobs (see extract_F_weights.py). When "coreml" it's the mlpackage path.
// demosaicMode: "metal-bilinear" (default) or "core-image" (CIRAWFilter).
// outResolution: "2k", "uhd", "4k", "6k", "8k" (default "8k"). Picks a fixed
// output width; output height preserves source aspect ratio (rounded to even).
// "8k" means "native source dims, no scale".
// cnnScale: "2x" (default — F variant, super-res) or "1x" (F_no_sr / BIBO_1x —
// clean bayer at codec dims; CIRAWFilter handles the upscale to outResolution).
// lookMode: nil/"none" by default. "mission1" applies the experimental Mission
// JPEG-look geometry/raw-filter path in the Core Image demosaic backend.
- (nullable instancetype)initWithFirstFrame:(NSString *)firstFrame
                                metaDngPath:(nullable NSString *)metaDngPath
                                   ckptPath:(NSString *)ckptPath
                                    outPath:(NSString *)outPath
                                        fps:(int)fps
                                       aaOn:(BOOL)aaOn
                                      noCNN:(BOOL)noCNN
                                    noCodec:(BOOL)noCodec
                                     timing:(BOOL)timing
                                 cnnBackend:(NSString *)cnnBackend
                                demosaicMode:(NSString *)demosaicMode
                              outResolution:(NSString *)outResolution
                                   cnnScale:(NSString *)cnnScale
                                   lookMode:(nullable NSString *)lookMode;

// Continue past per-frame errors (read/decode/CNN/demosaic failures). The
// failing frame's index is logged to stderr; subsequent frames continue. The
// final rc still reflects how many frames hit errors (non-zero if any). When
// NO, the first error aborts the run with rc != 0.
- (void)setSkipErrors:(BOOL)skip;

- (int)runFrames:(NSArray<NSString *> *)framePaths;

@end

NS_ASSUME_NONNULL_END
