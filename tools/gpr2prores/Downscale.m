// Downscale.m — host-side orchestrator for downscale_bilinear_bgra8.

#import "Downscale.h"

typedef struct {
    uint32_t inW;
    uint32_t inH;
    uint32_t outW;
    uint32_t outH;
} DownscaleParams;

@implementation Downscale {
    id<MTLDevice>                _device;
    id<MTLComputePipelineState>  _pso;
    uint32_t                     _inW, _inH, _outW, _outH;
    CVMetalTextureCacheRef       _texCache;
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                inWidth:(uint32_t)inW
                               inHeight:(uint32_t)inH
                               outWidth:(uint32_t)outW
                              outHeight:(uint32_t)outH
{
    self = [super init];
    if (!self) return nil;
    _device = device;
    _inW = inW; _inH = inH; _outW = outW; _outH = outH;

    NSError *err = nil;
    // Locate the metallib next to the binary (same scheme as Demosaic).
    NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
    NSString *libPath = [exeDir stringByAppendingPathComponent:@"default.metallib"];
    NSURL *libURL = [NSURL fileURLWithPath:libPath];
    id<MTLLibrary> lib = nil;
    if ([[NSFileManager defaultManager] fileExistsAtPath:libPath]) {
        lib = [_device newLibraryWithURL:libURL error:&err];
    }
    if (!lib) {
        NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
        libPath = [cwd stringByAppendingPathComponent:@"default.metallib"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:libPath]) {
            libURL = [NSURL fileURLWithPath:libPath];
            lib = [_device newLibraryWithURL:libURL error:&err];
        }
    }
    if (!lib) {
        fprintf(stderr, "Downscale: cannot load default.metallib: %s\n",
                err ? [err.localizedDescription UTF8String] : "(missing)");
        return nil;
    }

    id<MTLFunction> fn = [lib newFunctionWithName:@"downscale_bilinear_bgra8"];
    if (!fn) { fprintf(stderr, "Downscale: no downscale_bilinear_bgra8 fn\n"); return nil; }
    _pso = [_device newComputePipelineStateWithFunction:fn error:&err];
    if (!_pso) {
        fprintf(stderr, "Downscale: PSO error: %s\n", [err.localizedDescription UTF8String]);
        return nil;
    }

    CVMetalTextureCacheCreate(NULL, NULL, _device, NULL, &_texCache);
    fprintf(stderr, "Downscale: %ux%u → %ux%u (bilinear BGRA8)\n", inW, inH, outW, outH);
    return self;
}

- (void)dealloc {
    if (_texCache) CFRelease(_texCache);
}

- (void)encode:(id<MTLCommandBuffer>)cb
        inPixelBuffer:(CVPixelBufferRef)inPB
       outPixelBuffer:(CVPixelBufferRef)outPB
{
    // Wrap inPB as sample-able BGRA8 texture, outPB as write-only BGRA8.
    CVMetalTextureRef inTexRef = NULL, outTexRef = NULL;
    size_t inW  = CVPixelBufferGetWidth(inPB);
    size_t inH  = CVPixelBufferGetHeight(inPB);
    size_t outW = CVPixelBufferGetWidth(outPB);
    size_t outH = CVPixelBufferGetHeight(outPB);

    CVMetalTextureCacheCreateTextureFromImage(NULL, _texCache, inPB, NULL,
                                              MTLPixelFormatBGRA8Unorm,
                                              inW, inH, 0, &inTexRef);
    CVMetalTextureCacheCreateTextureFromImage(NULL, _texCache, outPB, NULL,
                                              MTLPixelFormatBGRA8Unorm,
                                              outW, outH, 0, &outTexRef);
    if (!inTexRef || !outTexRef) {
        fprintf(stderr, "Downscale: tex wrap failed\n");
        if (inTexRef) CFRelease(inTexRef);
        if (outTexRef) CFRelease(outTexRef);
        return;
    }
    id<MTLTexture> inTex  = CVMetalTextureGetTexture(inTexRef);
    id<MTLTexture> outTex = CVMetalTextureGetTexture(outTexRef);

    DownscaleParams P = {
        .inW  = (uint32_t)inW,
        .inH  = (uint32_t)inH,
        .outW = (uint32_t)outW,
        .outH = (uint32_t)outH,
    };

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:_pso];
    [enc setTexture:inTex  atIndex:0];
    [enc setTexture:outTex atIndex:1];
    [enc setBytes:&P length:sizeof(P) atIndex:0];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    MTLSize gg = MTLSizeMake((outW + tg.width - 1) / tg.width,
                             (outH + tg.height - 1) / tg.height, 1);
    [enc dispatchThreadgroups:gg threadsPerThreadgroup:tg];
    [enc endEncoding];

    CFRelease(inTexRef);
    CFRelease(outTexRef);
}

@end
