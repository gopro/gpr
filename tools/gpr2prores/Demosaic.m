// Demosaic.m — host-side Metal kernel orchestrator.

#import "Demosaic.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t cfaPattern;
    uint32_t blackLevel;
    float    whiteScale;
    float    gammaInvA;
    float    gammaInvB;
    float    wbR, wbG, wbB;
    float    gainR, gainG, gainB;
    float    m00, m01, m02;
    float    m10, m11, m12;
    float    m20, m21, m22;
} DemosaicParams;

@implementation Demosaic {
    id<MTLDevice>             _device;
    id<MTLComputePipelineState> _pso;
    id<MTLBuffer>             _bayerBuf;
    size_t                    _bayerCap;
    uint32_t                  _width, _height;
    uint32_t                  _cfaPattern;
    uint32_t                  _blackLevel;
    uint32_t                  _whiteLevel;
    float                     _wbR, _wbG, _wbB;
    float                     _rgbCam[3][3];
    CVMetalTextureCacheRef    _texCache;
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                                  width:(uint32_t)w
                                 height:(uint32_t)h
                             cfaPattern:(uint32_t)cfaPattern
                             blackLevel:(uint32_t)blackLevel
                             whiteLevel:(uint32_t)whiteLevel
                                    wbR:(float)wbR
                                    wbG:(float)wbG
                                    wbB:(float)wbB
                                 rgbCam:(const float (*)[3])rgbCam
{
    self = [super init];
    if (!self) return nil;
    _device = device;
    _width = w;
    _height = h;
    _cfaPattern = cfaPattern;
    _blackLevel = blackLevel;
    _whiteLevel = whiteLevel;
    _wbR = wbR; _wbG = wbG; _wbB = wbB;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            _rgbCam[r][c] = rgbCam[r][c];

    NSError *err = nil;
    // Locate the metallib next to the binary
    NSString *exeDir = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
    NSString *libPath = [exeDir stringByAppendingPathComponent:@"default.metallib"];
    NSURL *libURL = [NSURL fileURLWithPath:libPath];
    id<MTLLibrary> lib = nil;
    if ([[NSFileManager defaultManager] fileExistsAtPath:libPath]) {
        lib = [_device newLibraryWithURL:libURL error:&err];
    }
    if (!lib) {
        // Fall back to cwd
        NSString *cwd = [[NSFileManager defaultManager] currentDirectoryPath];
        libPath = [cwd stringByAppendingPathComponent:@"default.metallib"];
        if ([[NSFileManager defaultManager] fileExistsAtPath:libPath]) {
            libURL = [NSURL fileURLWithPath:libPath];
            lib = [_device newLibraryWithURL:libURL error:&err];
        }
    }
    if (!lib) {
        fprintf(stderr, "Demosaic: cannot load default.metallib: %s\n",
                err ? [err.localizedDescription UTF8String] : "(missing)");
        return nil;
    }

    id<MTLFunction> fn = [lib newFunctionWithName:@"demosaic_bilinear"];
    if (!fn) { fprintf(stderr, "no demosaic_bilinear fn\n"); return nil; }
    _pso = [_device newComputePipelineStateWithFunction:fn error:&err];
    if (!_pso) {
        fprintf(stderr, "PSO error: %s\n", [err.localizedDescription UTF8String]);
        return nil;
    }

    _bayerCap = (size_t)w * h * sizeof(uint16_t);
    _bayerBuf = [_device newBufferWithLength:_bayerCap
                                     options:MTLResourceStorageModeShared];

    CVMetalTextureCacheCreate(NULL, NULL, _device, NULL, &_texCache);

    return self;
}

- (void)encode:(id<MTLCommandBuffer>)cb
         bayer:(const uint16_t *)bayer
         width:(uint32_t)w
        height:(uint32_t)h
outPixelBuffer:(CVPixelBufferRef)pb
{
    size_t needed = (size_t)w * h * sizeof(uint16_t);
    if (needed > _bayerCap) {
        _bayerBuf = [_device newBufferWithLength:needed
                                         options:MTLResourceStorageModeShared];
        _bayerCap = needed;
    }
    memcpy(_bayerBuf.contents, bayer, needed);

    // Wrap CVPixelBuffer as an MTLTexture
    CVMetalTextureRef texRef = NULL;
    OSType pf = CVPixelBufferGetPixelFormatType(pb);
    MTLPixelFormat mpf = (pf == kCVPixelFormatType_32BGRA) ? MTLPixelFormatBGRA8Unorm
                                                          : MTLPixelFormatBGRA8Unorm;
    CVMetalTextureCacheCreateTextureFromImage(NULL, _texCache, pb, NULL,
                                              mpf, w, h, 0, &texRef);
    if (!texRef) {
        fprintf(stderr, "Demosaic: tex from pixel buffer failed\n");
        return;
    }
    id<MTLTexture> outTex = CVMetalTextureGetTexture(texRef);

    // White balance multipliers from DNG cam_mul (Phase 4+).
    float wbR = _wbR, wbG = _wbG, wbB = _wbB;
    DemosaicParams P;
    P.width = w; P.height = h;
    P.cfaPattern = _cfaPattern;
    P.blackLevel = _blackLevel;
    float wl = (float)_whiteLevel - (float)_blackLevel;
    P.whiteScale = (wl > 0) ? (1.0f / wl) : (1.0f / 16383.0f);
    P.gammaInvA = 2.222f; P.gammaInvB = 4.5f;
    P.wbR = wbR; P.wbG = wbG; P.wbB = wbB;
    P.gainR = 1.0f; P.gainG = 1.0f; P.gainB = 1.0f;
    P.m00 = _rgbCam[0][0]; P.m01 = _rgbCam[0][1]; P.m02 = _rgbCam[0][2];
    P.m10 = _rgbCam[1][0]; P.m11 = _rgbCam[1][1]; P.m12 = _rgbCam[1][2];
    P.m20 = _rgbCam[2][0]; P.m21 = _rgbCam[2][1]; P.m22 = _rgbCam[2][2];

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:_pso];
    [enc setBuffer:_bayerBuf offset:0 atIndex:0];
    [enc setBytes:&P length:sizeof(P) atIndex:1];
    [enc setTexture:outTex atIndex:0];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    MTLSize gg = MTLSizeMake((w + tg.width - 1) / tg.width,
                             (h + tg.height - 1) / tg.height, 1);
    [enc dispatchThreadgroups:gg threadsPerThreadgroup:tg];
    [enc endEncoding];

    CFRelease(texRef);
}

@end
