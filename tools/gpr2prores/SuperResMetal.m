// SuperResMetal.m — MPSGraph-based F super-res CNN.
//
// Architecture: F (NAFNetTiny w=16, 3-level UNet, 263K params) + PixelShuffle
// 2× SR head + bicubic baseline. Weights are loaded from a directory of fp16
// .bin blobs (see extract_F_weights.py).
//
// The graph runs in NHWC fp16. Inputs/outputs are MTLBuffers we own.
//
// Implementation notes:
//   - Weight tensors are stored in PyTorch's OIHW layout in the .bin files
//     and reshaped/transposed once at load time into HWIO (the layout
//     MPSGraph expects when weightsLayout=HWIO).
//   - LayerNorm2d across channels is implemented via mean/variance over
//     axis=-1 (the channel axis in NHWC), followed by normalization.
//   - SimpleGate splits the 2C channel into two C-channel halves and
//     multiplies them.
//   - PixelShuffle is implemented as depthToSpace2DTensor with the channel
//     (last) axis and block size 2.
//   - The bicubic baseline is computed in scalar/dispatch_apply for simplicity
//     in this revision; a Metal kernel can replace it later.

#import "SuperResMetal.h"
#import <Accelerate/Accelerate.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>
#import <mach/mach_time.h>

// ============================================================================
// Helpers
// ============================================================================

static inline double now_ms_local(void) {
    static mach_timebase_info_data_t tb = {0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1.0e6;
}

static void *read_file_to_mem(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "SuperResMetal: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n);
    size_t r = fread(buf, 1, n, f);
    fclose(f);
    if ((long)r != n) { free(buf); return NULL; }
    if (out_size) *out_size = (size_t)n;
    return buf;
}

// Transpose OIHW -> HWIO for NHWC conv (or [Cout,Cin] -> [Cin,Cout] for 1x1)
// in-place on a fresh buffer. Returns a newly malloc'd HWIO buffer.
// shape_in is (Cout, Cin, kH, kW); output is (kH, kW, Cin, Cout) flattened.
static uint16_t *transpose_oihw_to_hwio_f16(const uint16_t *src,
                                            uint32_t Cout, uint32_t Cin,
                                            uint32_t kH, uint32_t kW) {
    uint16_t *dst = malloc((size_t)kH * kW * Cin * Cout * sizeof(uint16_t));
    for (uint32_t o = 0; o < Cout; o++) {
        for (uint32_t i = 0; i < Cin; i++) {
            for (uint32_t y = 0; y < kH; y++) {
                for (uint32_t x = 0; x < kW; x++) {
                    uint32_t src_idx = ((o * Cin + i) * kH + y) * kW + x;
                    uint32_t dst_idx = ((y * kW + x) * Cin + i) * Cout + o;
                    dst[dst_idx] = src[src_idx];
                }
            }
        }
    }
    return dst;
}

// For depthwise conv 2D in MPSGraph with weightsLayout HWIO, the expected
// weights layout for grouped DW with groups==C is (kH, kW, 1, C) — same as
// HWIO with input-channels=1.
// PyTorch stores it as (C, 1, kH, kW). We need (kH, kW, 1, C).
static uint16_t *transpose_dw_pytorch_to_hwio_f16(const uint16_t *src_oihw_2c,
                                                  uint32_t Cout,
                                                  uint32_t kH, uint32_t kW) {
    // src layout PyTorch: [Cout, 1, kH, kW]
    // dst layout HWIO:    [kH, kW, 1, Cout]
    uint16_t *dst = malloc((size_t)kH * kW * 1 * Cout * sizeof(uint16_t));
    for (uint32_t o = 0; o < Cout; o++) {
        for (uint32_t y = 0; y < kH; y++) {
            for (uint32_t x = 0; x < kW; x++) {
                uint32_t src_idx = (o * kH + y) * kW + x;
                uint32_t dst_idx = (y * kW + x) * Cout + o;
                dst[dst_idx] = src_oihw_2c[src_idx];
            }
        }
    }
    return dst;
}

// ============================================================================
// Weight bank — holds raw .bin contents as MTLBuffers, layout-translated.
// ============================================================================
@interface SuperResMetalWeights : NSObject
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) NSMutableDictionary<NSString *, id<MTLBuffer>> *buffers;
@end

@implementation SuperResMetalWeights

- (instancetype)initWithDevice:(id<MTLDevice>)device {
    self = [super init];
    if (!self) return nil;
    _device = device;
    _buffers = [NSMutableDictionary dictionary];
    return self;
}

// Load a raw fp16 .bin and store it in the dictionary keyed by `name`.
// No layout change.
- (BOOL)loadRaw:(NSString *)name fromPath:(NSString *)path {
    size_t n = 0;
    void *raw = read_file_to_mem([path UTF8String], &n);
    if (!raw) { return NO; }
    id<MTLBuffer> b = [_device newBufferWithBytes:raw length:n options:MTLResourceStorageModeShared];
    free(raw);
    if (!b) return NO;
    _buffers[name] = b;
    return YES;
}

// Load a 4D conv weight (Cout, Cin, kH, kW) fp16 and store as HWIO.
- (BOOL)loadConv4D:(NSString *)name fromPath:(NSString *)path
              Cout:(uint32_t)Cout Cin:(uint32_t)Cin kH:(uint32_t)kH kW:(uint32_t)kW
{
    size_t n = 0;
    uint16_t *raw = (uint16_t *)read_file_to_mem([path UTF8String], &n);
    if (!raw) return NO;
    size_t expected = (size_t)Cout * Cin * kH * kW * sizeof(uint16_t);
    if (n != expected) {
        fprintf(stderr, "SuperResMetal: %s size %zu != expected %zu\n",
                [path UTF8String], n, expected);
        free(raw);
        return NO;
    }
    uint16_t *hwio = transpose_oihw_to_hwio_f16(raw, Cout, Cin, kH, kW);
    free(raw);
    id<MTLBuffer> b = [_device newBufferWithBytes:hwio
                                            length:expected
                                          options:MTLResourceStorageModeShared];
    free(hwio);
    if (!b) return NO;
    _buffers[name] = b;
    return YES;
}

// Load a 4D conv weight (Cout, Cin, kH, kW) fp16 and store TWO buffers:
//   key       -> HWIO  (for MPSGraph)
//   key+"_raw" -> raw  Cout × Cin × kH × kW  row-major (for Metal kernels: [Cout, Cin] when kH=kW=1)
- (BOOL)loadConv4DBoth:(NSString *)name fromPath:(NSString *)path
                  Cout:(uint32_t)Cout Cin:(uint32_t)Cin kH:(uint32_t)kH kW:(uint32_t)kW
{
    size_t n = 0;
    uint16_t *raw = (uint16_t *)read_file_to_mem([path UTF8String], &n);
    if (!raw) return NO;
    size_t expected = (size_t)Cout * Cin * kH * kW * sizeof(uint16_t);
    if (n != expected) {
        fprintf(stderr, "SuperResMetal: %s size %zu != expected %zu\n",
                [path UTF8String], n, expected);
        free(raw);
        return NO;
    }
    // raw copy (used by Metal kernels)
    id<MTLBuffer> rawBuffer = [_device newBufferWithBytes:raw length:expected
                                                   options:MTLResourceStorageModeShared];
    if (!rawBuffer) { free(raw); return NO; }
    _buffers[[name stringByAppendingString:@"_raw"]] = rawBuffer;
    // hwio copy (used by MPSGraph)
    uint16_t *hwio = transpose_oihw_to_hwio_f16(raw, Cout, Cin, kH, kW);
    free(raw);
    id<MTLBuffer> b = [_device newBufferWithBytes:hwio length:expected
                                          options:MTLResourceStorageModeShared];
    free(hwio);
    if (!b) return NO;
    _buffers[name] = b;
    return YES;
}

// Load a 4D depthwise conv weight (Cout, 1, kH, kW) and store as HWIO (kH, kW, 1, Cout).
- (BOOL)loadDW:(NSString *)name fromPath:(NSString *)path
          Cout:(uint32_t)Cout kH:(uint32_t)kH kW:(uint32_t)kW
{
    size_t n = 0;
    uint16_t *raw = (uint16_t *)read_file_to_mem([path UTF8String], &n);
    if (!raw) return NO;
    size_t expected = (size_t)Cout * kH * kW * sizeof(uint16_t);
    if (n != expected) {
        fprintf(stderr, "SuperResMetal: %s size %zu != expected %zu\n",
                [path UTF8String], n, expected);
        free(raw);
        return NO;
    }
    // Note: extract_F_weights.py reshapes dw to [2C, 9] (drops the input-channels=1 dim)
    uint16_t *hwio = transpose_dw_pytorch_to_hwio_f16(raw, Cout, kH, kW);
    free(raw);
    id<MTLBuffer> b = [_device newBufferWithBytes:hwio
                                            length:expected
                                          options:MTLResourceStorageModeShared];
    free(hwio);
    if (!b) return NO;
    _buffers[name] = b;
    return YES;
}

// Load DW weight and keep both:
//   key       -> HWIO  (for MPSGraph)
//   key+"_raw" -> [2C, 9] row-major (for Metal kernels — same as PyTorch native)
- (BOOL)loadDWBoth:(NSString *)name fromPath:(NSString *)path
              Cout:(uint32_t)Cout kH:(uint32_t)kH kW:(uint32_t)kW
{
    size_t n = 0;
    uint16_t *raw = (uint16_t *)read_file_to_mem([path UTF8String], &n);
    if (!raw) return NO;
    size_t expected = (size_t)Cout * kH * kW * sizeof(uint16_t);
    if (n != expected) {
        fprintf(stderr, "SuperResMetal: %s size %zu != expected %zu\n",
                [path UTF8String], n, expected);
        free(raw);
        return NO;
    }
    id<MTLBuffer> rawBuffer = [_device newBufferWithBytes:raw length:expected
                                                   options:MTLResourceStorageModeShared];
    if (!rawBuffer) { free(raw); return NO; }
    _buffers[[name stringByAppendingString:@"_raw"]] = rawBuffer;
    uint16_t *hwio = transpose_dw_pytorch_to_hwio_f16(raw, Cout, kH, kW);
    free(raw);
    id<MTLBuffer> b = [_device newBufferWithBytes:hwio length:expected
                                          options:MTLResourceStorageModeShared];
    free(hwio);
    if (!b) return NO;
    _buffers[name] = b;
    return YES;
}

@end

// ============================================================================
// SuperResMetal — main class
// ============================================================================

@interface SuperResMetal () {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    MPSGraphDevice *_mpsDevice;
    MPSGraph *_graph;
    MPSGraphTensor *_inputTensor;    // (1, Hp, Wp, 4) fp16 placeholder
    MPSGraphTensor *_residualTensor; // (1, 2Hp, 2Wp, 4) fp16 SR head output
    // Pre-compiled executable for the MPSGraph backend. Built once at init
    // (or lazily on first frame) to avoid per-frame compile/specialization
    // work that the encodeToCommandBuffer: feeds: path may otherwise repeat.
    MPSGraphExecutable *_graphExec;
    MPSGraphExecutableExecutionDescriptor *_graphExecDesc;
    SuperResMetalWeights *_W;
    SuperResMetalBackend _backend;
    BOOL _useSubpixelHead;  // YES = F (2× SR); NO = F_no_sr (1× outro head)
    BOOL _isAneModel;       // YES = F_ane (BN-folded into Conv1x1, SiLU). NO = legacy F (LN+SimpleGate)
    uint32_t _dwKernel;     // 3 or 7. For F_ane only; legacy F is always 3.
    uint32_t _modelWidth;   // Detected from intro_weight.bin file size (16, 24, 32 …).
                            // All layer widths in this codebase scale as
                            // {width, 2*width, 4*width} for enc/dec and 8*width
                            // for the middle. The Metal-hybrid backend only
                            // supports width=16 because the kernels' v[128]
                            // register array can't fit C_PJ_IN > 128.

    // Persistent buffers.
    id<MTLBuffer> _inBayer;       // input Bayer uint16, sized for max codec output
    size_t _inBayerCap;
    id<MTLBuffer> _outBayer;      // output Bayer uint16
    size_t _outBayerCap;
    id<MTLBuffer> _inBuf;         // NHWC fp16 planes (graph input)
    id<MTLBuffer> _outBuf;        // NHWC fp16 residual (graph output)
    id<MTLBuffer> _baselineBuf;   // NHWC fp16 bicubic baseline
    uint32_t _Hp, _Wp;

    // Post-processing kernels.
    id<MTLComputePipelineState> _psoUnpack;
    id<MTLComputePipelineState> _psoBicubic;
    id<MTLComputePipelineState> _psoCombine;
    id<MTLComputePipelineState> _psoBicubicCombine;  // fused bicubic+combine+rebayer (2× mode)
    id<MTLComputePipelineState> _psoCombine1x;       // combine+rebayer (1× mode, no bicubic)

    // -- Hybrid path state (only used when _backend == Hybrid) --
    // 7 sub-graphs with feed placeholders and result tensors.
    MPSGraph *_gIntro;         MPSGraphTensor *_gIntroIn, *_gIntroOut;
    MPSGraph *_gDown0;         MPSGraphTensor *_gDown0In, *_gDown0Out;
    MPSGraph *_gDown1;         MPSGraphTensor *_gDown1In, *_gDown1Out;
    MPSGraph *_gMid;           MPSGraphTensor *_gMidIn, *_gMidSkip2, *_gMidOutDec0In;
    MPSGraph *_gUp1;           MPSGraphTensor *_gUp1In, *_gUp1Skip1, *_gUp1Out;
    MPSGraph *_gUp2;           MPSGraphTensor *_gUp2In, *_gUp2Skip0, *_gUp2Out;
    MPSGraph *_gHead;          MPSGraphTensor *_gHeadIn, *_gHeadOut;

    // Intermediate NHWC fp16 buffers between graph segments and NAFBlocks.
    // Sizes are fixed by _Hp, _Wp (network input plane dims).
    id<MTLBuffer> _bEnc0In;              // [Hp, Wp, 16]
    id<MTLBuffer> _bEnc0Out;             // [Hp, Wp, 16]    (==skip0)
    id<MTLBuffer> _bEnc1In;              // [Hp/2, Wp/2, 32]
    id<MTLBuffer> _bEnc1Out;             // [Hp/2, Wp/2, 32] (==skip1)
    id<MTLBuffer> _bEnc2In;              // [Hp/4, Wp/4, 64]
    id<MTLBuffer> _bEnc2Out;             // [Hp/4, Wp/4, 64] (==skip2)
    id<MTLBuffer> _bDec0In;              // [Hp/4, Wp/4, 64] (from down2+mid+up0+skip2)
    id<MTLBuffer> _bDec0Out;             // [Hp/4, Wp/4, 64]
    id<MTLBuffer> _bDec1In;              // [Hp/2, Wp/2, 32]
    id<MTLBuffer> _bDec1Out;             // [Hp/2, Wp/2, 32]
    id<MTLBuffer> _bDec2In;              // [Hp, Wp, 16]
    id<MTLBuffer> _bDec2Out;             // [Hp, Wp, 16]

    // Per-NAFBlock scratch buffers (z1, m, z2). Sized for max NAFBlock memory.
    // We need one set per concurrent block — but since blocks execute serially
    // in a single command buffer, we can reuse scratch across blocks at the
    // same C×spatial size. Worst case is C=64 at H/4 × W/4 (== enc2/dec0).
    // For correctness we allocate separate scratch per level, since the GPU
    // command buffer may schedule between levels but each block needs its own
    // intermediates valid until that block completes.
    // Layout: 3 buffers per block × 6 blocks (enc0, enc1, enc2, dec0, dec1, dec2).
    id<MTLBuffer> _scratchZ1[6];   // pre-DW [H, W, 2C]
    id<MTLBuffer> _scratchM[6];    // mid (after half-1) [H, W, C]
    id<MTLBuffer> _scratchZ2[6];   // pre-gate2 [H, W, 2C]

    // PSOs for the validated kernels, keyed by level index (0=C16, 1=C32, 2=C64).
    // We need 4 dispatches per block; in-block they are:
    //   stage1: fused_ln_conv1x1 (C -> 2C)
    //   stage2: fused_dw_gate_proj_res (DW + Gate + Proj + residual to mid)
    //   stage3: fused_ln_conv1x1 (C -> 2C) -- can reuse stage1 PSO (same C)
    //   stage4: fused_gate_proj_res (Gate + Proj + residual to out)
    id<MTLComputePipelineState> _psoLN[3];     // [0]=C16, [1]=C32, [2]=C64
    id<MTLComputePipelineState> _psoDGR[3];
    id<MTLComputePipelineState> _psoGPR[3];

    // F_ane PSOs (parallel to LN/DGR/GPR, used when _isAneModel == YES).
    //   stage1: fused_ane_conv1x1 (C -> 2C)             -- BN1+conv1 folded
    //   stage2: fused_ane_dw_silu_proj_res              -- DW(k×k)+SiLU+proj+res
    //   stage3: fused_ane_conv1x1 (C -> 2C)             -- BN2+mlp1 folded, reuse pso
    //   stage4: fused_ane_silu_proj_res                 -- SiLU+proj+res
    id<MTLComputePipelineState> _psoAneConv1x1[3];
    id<MTLComputePipelineState> _psoAneDSPR[3];
    id<MTLComputePipelineState> _psoAneSPR[3];

    // Spatial dims per level (in_plane H/W >> level).
    uint32_t _levelH[3], _levelW[3];
}
@end

@implementation SuperResMetal

// ---------- Graph builder helpers (NHWC) ----------

// Create a placeholder + constant tensor for biases or LN params (rank 1, length C).
static MPSGraphTensor *constTensor1D(MPSGraph *g, id<MTLBuffer> b, NSUInteger C) {
    NSData *data = [NSData dataWithBytesNoCopy:b.contents length:C * sizeof(uint16_t) freeWhenDone:NO];
    return [g constantWithData:data shape:@[@(C)] dataType:MPSDataTypeFloat16];
}

// Constant for HWIO conv weights.
static MPSGraphTensor *constTensorHWIO(MPSGraph *g, id<MTLBuffer> b,
                                       NSUInteger kH, NSUInteger kW,
                                       NSUInteger Cin, NSUInteger Cout) {
    NSUInteger n = kH * kW * Cin * Cout;
    NSData *data = [NSData dataWithBytesNoCopy:b.contents length:n * sizeof(uint16_t) freeWhenDone:NO];
    return [g constantWithData:data shape:@[@(kH), @(kW), @(Cin), @(Cout)] dataType:MPSDataTypeFloat16];
}

// Constant for HWIO depthwise weights (kH, kW, 1, C).
static MPSGraphTensor *constTensorDW_HWIO(MPSGraph *g, id<MTLBuffer> b,
                                          NSUInteger kH, NSUInteger kW, NSUInteger C) {
    NSUInteger n = kH * kW * 1 * C;
    NSData *data = [NSData dataWithBytesNoCopy:b.contents length:n * sizeof(uint16_t) freeWhenDone:NO];
    return [g constantWithData:data shape:@[@(kH), @(kW), @1, @(C)] dataType:MPSDataTypeFloat16];
}

// Conv2D NHWC with stride 1, "same" padding.
static MPSGraphTensor *conv2d_NHWC(MPSGraph *g, MPSGraphTensor *x, MPSGraphTensor *w,
                                   MPSGraphTensor *_Nullable b,
                                   NSUInteger stride, NSUInteger pad,
                                   NSString *name)
{
    MPSGraphConvolution2DOpDescriptor *desc =
        [MPSGraphConvolution2DOpDescriptor descriptorWithStrideInX:stride
                                                          strideInY:stride
                                                    dilationRateInX:1
                                                    dilationRateInY:1
                                                             groups:1
                                                        paddingLeft:pad
                                                       paddingRight:pad
                                                         paddingTop:pad
                                                      paddingBottom:pad
                                                       paddingStyle:MPSGraphPaddingStyleExplicit
                                                         dataLayout:MPSGraphTensorNamedDataLayoutNHWC
                                                      weightsLayout:MPSGraphTensorNamedDataLayoutHWIO];
    MPSGraphTensor *y = [g convolution2DWithSourceTensor:x weightsTensor:w descriptor:desc name:[name stringByAppendingString:@"_conv"]];
    if (b) {
        // bias is rank-1 (C,). Reshape to (1,1,1,C) for NHWC broadcast.
        MPSGraphTensor *brc = [g reshapeTensor:b withShape:@[@1, @1, @1, b.shape.lastObject]
                                          name:[name stringByAppendingString:@"_bias_rs"]];
        y = [g additionWithPrimaryTensor:y secondaryTensor:brc name:[name stringByAppendingString:@"_bias_add"]];
    }
    return y;
}

// Depthwise conv 2D NHWC stride 1, padding 1 (3x3).
static MPSGraphTensor *dwconv_NHWC(MPSGraph *g, MPSGraphTensor *x, MPSGraphTensor *w,
                                   MPSGraphTensor *_Nullable b, NSString *name) {
    MPSGraphDepthwiseConvolution2DOpDescriptor *desc =
        [MPSGraphDepthwiseConvolution2DOpDescriptor
            descriptorWithDataLayout:MPSGraphTensorNamedDataLayoutNHWC
                      weightsLayout:MPSGraphTensorNamedDataLayoutHWIO];
    desc.strideInX = 1;
    desc.strideInY = 1;
    desc.paddingLeft = 1;
    desc.paddingRight = 1;
    desc.paddingTop = 1;
    desc.paddingBottom = 1;
    desc.paddingStyle = MPSGraphPaddingStyleExplicit;
    MPSGraphTensor *y = [g depthwiseConvolution2DWithSourceTensor:x weightsTensor:w descriptor:desc name:[name stringByAppendingString:@"_dw"]];
    if (b) {
        MPSGraphTensor *brc = [g reshapeTensor:b withShape:@[@1, @1, @1, b.shape.lastObject]
                                          name:[name stringByAppendingString:@"_dwbias_rs"]];
        y = [g additionWithPrimaryTensor:y secondaryTensor:brc name:[name stringByAppendingString:@"_dwbias_add"]];
    }
    return y;
}

// Depthwise conv 2D NHWC stride 1, arbitrary kernel size (pad = k/2).
static MPSGraphTensor *dwconvK_NHWC(MPSGraph *g, MPSGraphTensor *x, MPSGraphTensor *w,
                                    MPSGraphTensor *_Nullable b, NSUInteger k, NSString *name) {
    NSUInteger pad = k / 2;
    MPSGraphDepthwiseConvolution2DOpDescriptor *desc =
        [MPSGraphDepthwiseConvolution2DOpDescriptor
            descriptorWithDataLayout:MPSGraphTensorNamedDataLayoutNHWC
                      weightsLayout:MPSGraphTensorNamedDataLayoutHWIO];
    desc.strideInX = 1;
    desc.strideInY = 1;
    desc.paddingLeft = pad;
    desc.paddingRight = pad;
    desc.paddingTop = pad;
    desc.paddingBottom = pad;
    desc.paddingStyle = MPSGraphPaddingStyleExplicit;
    MPSGraphTensor *y = [g depthwiseConvolution2DWithSourceTensor:x weightsTensor:w descriptor:desc name:[name stringByAppendingString:@"_dw"]];
    if (b) {
        MPSGraphTensor *brc = [g reshapeTensor:b withShape:@[@1, @1, @1, b.shape.lastObject]
                                          name:[name stringByAppendingString:@"_dwbias_rs"]];
        y = [g additionWithPrimaryTensor:y secondaryTensor:brc name:[name stringByAppendingString:@"_dwbias_add"]];
    }
    return y;
}

// SiLU activation: x * sigmoid(x). NHWC.
static MPSGraphTensor *silu_NHWC(MPSGraph *g, MPSGraphTensor *x, NSString *name) {
    MPSGraphTensor *s = [g sigmoidWithTensor:x name:[name stringByAppendingString:@"_sig"]];
    return [g multiplicationWithPrimaryTensor:x secondaryTensor:s
                                          name:[name stringByAppendingString:@"_silu"]];
}

// LayerNorm2d over the channel axis (axis=-1 in NHWC). Affine.
static MPSGraphTensor *layernorm2d_NHWC(MPSGraph *g, MPSGraphTensor *x,
                                        MPSGraphTensor *gamma, MPSGraphTensor *beta,
                                        NSString *name) {
    MPSGraphTensor *mean = [g meanOfTensor:x axes:@[@3] name:[name stringByAppendingString:@"_mean"]];
    MPSGraphTensor *var  = [g varianceOfTensor:x meanTensor:mean axes:@[@3] name:[name stringByAppendingString:@"_var"]];
    // gamma/beta are (C,). Reshape to (1,1,1,C) for broadcast.
    MPSGraphTensor *grc = [g reshapeTensor:gamma withShape:@[@1, @1, @1, gamma.shape.lastObject]
                                      name:[name stringByAppendingString:@"_grc"]];
    MPSGraphTensor *brc = [g reshapeTensor:beta withShape:@[@1, @1, @1, beta.shape.lastObject]
                                      name:[name stringByAppendingString:@"_brc"]];
    return [g normalizationWithTensor:x meanTensor:mean varianceTensor:var
                          gammaTensor:grc betaTensor:brc
                              epsilon:1e-6f name:[name stringByAppendingString:@"_ln"]];
}

// SimpleGate: split last dim in half, multiply.
// Input (N,H,W,2C) → (N,H,W,C)
static MPSGraphTensor *simpleGate_NHWC(MPSGraph *g, MPSGraphTensor *x, NSString *name) {
    NSArray<NSNumber *> *shape = x.shape;
    NSUInteger twoC = shape.lastObject.unsignedIntegerValue;
    NSUInteger C = twoC / 2;
    MPSGraphTensor *a = [g sliceTensor:x dimension:3 start:0 length:C name:[name stringByAppendingString:@"_a"]];
    MPSGraphTensor *b = [g sliceTensor:x dimension:3 start:C length:C name:[name stringByAppendingString:@"_b"]];
    return [g multiplicationWithPrimaryTensor:a secondaryTensor:b name:[name stringByAppendingString:@"_gate"]];
}

// NAFBlock in MPSGraph NHWC. Returns the output tensor of the block (same shape as input).
typedef struct {
    NSString *gamma1;  // (C,)
    NSString *beta1;
    NSString *W1;      // conv1 (HWIO 1,1,C,2C)
    NSString *B1;      // (2C,)
    NSString *DWw;     // dw HWIO (3,3,1,2C)
    NSString *DWb;     // (2C,)
    NSString *Pj1w;    // proj1 HWIO (1,1,C,C)
    NSString *Pj1b;    // (C,)
    NSString *gamma2;
    NSString *beta2;
    NSString *W2;      // mlp1 HWIO (1,1,C,2C)
    NSString *B2;      // (2C,)
    NSString *Pj2w;    // mlp2 HWIO (1,1,C,C)
    NSString *Pj2b;    // (C,)
} NAFBlockNames;

- (MPSGraphTensor *)buildNAFBlock:(MPSGraphTensor *)x
                              C:(NSUInteger)C
                          names:(NAFBlockNames)n
                       basename:(NSString *)basename
{
    MPSGraph *g = _graph;
    // half-1: y = LN1(x), conv1, dw, gate, proj1 -> add to x
    MPSGraphTensor *gamma1 = constTensor1D(g, _W.buffers[n.gamma1], C);
    MPSGraphTensor *beta1  = constTensor1D(g, _W.buffers[n.beta1],  C);
    MPSGraphTensor *W1     = constTensorHWIO(g, _W.buffers[n.W1], 1, 1, C, 2*C);
    MPSGraphTensor *B1     = constTensor1D(g, _W.buffers[n.B1], 2*C);
    MPSGraphTensor *DWw    = constTensorDW_HWIO(g, _W.buffers[n.DWw], 3, 3, 2*C);
    MPSGraphTensor *DWb    = constTensor1D(g, _W.buffers[n.DWb], 2*C);
    MPSGraphTensor *Pj1w   = constTensorHWIO(g, _W.buffers[n.Pj1w], 1, 1, C, C);
    MPSGraphTensor *Pj1b   = constTensor1D(g, _W.buffers[n.Pj1b], C);

    MPSGraphTensor *y = layernorm2d_NHWC(g, x, gamma1, beta1, [basename stringByAppendingString:@"_ln1"]);
    y = conv2d_NHWC(g, y, W1, B1, 1, 0, [basename stringByAppendingString:@"_conv1"]);
    y = dwconv_NHWC(g, y, DWw, DWb, [basename stringByAppendingString:@"_dw"]);
    y = simpleGate_NHWC(g, y, [basename stringByAppendingString:@"_sg1"]);
    y = conv2d_NHWC(g, y, Pj1w, Pj1b, 1, 0, [basename stringByAppendingString:@"_proj1"]);
    x = [g additionWithPrimaryTensor:x secondaryTensor:y name:[basename stringByAppendingString:@"_res1"]];

    // half-2: y = LN2(x), mlp1, gate, mlp2 -> add to x
    MPSGraphTensor *gamma2 = constTensor1D(g, _W.buffers[n.gamma2], C);
    MPSGraphTensor *beta2  = constTensor1D(g, _W.buffers[n.beta2],  C);
    MPSGraphTensor *W2     = constTensorHWIO(g, _W.buffers[n.W2], 1, 1, C, 2*C);
    MPSGraphTensor *B2     = constTensor1D(g, _W.buffers[n.B2], 2*C);
    MPSGraphTensor *Pj2w   = constTensorHWIO(g, _W.buffers[n.Pj2w], 1, 1, C, C);
    MPSGraphTensor *Pj2b   = constTensor1D(g, _W.buffers[n.Pj2b], C);

    y = layernorm2d_NHWC(g, x, gamma2, beta2, [basename stringByAppendingString:@"_ln2"]);
    y = conv2d_NHWC(g, y, W2, B2, 1, 0, [basename stringByAppendingString:@"_mlp1"]);
    y = simpleGate_NHWC(g, y, [basename stringByAppendingString:@"_sg2"]);
    y = conv2d_NHWC(g, y, Pj2w, Pj2b, 1, 0, [basename stringByAppendingString:@"_mlp2"]);
    x = [g additionWithPrimaryTensor:x secondaryTensor:y name:[basename stringByAppendingString:@"_res2"]];

    return x;
}

// Helper: NAFBlock with a name prefix (enc0/enc1/enc2/middle/dec0/dec1/dec2).
- (MPSGraphTensor *)nafblockNamed:(NSString *)prefix C:(NSUInteger)C input:(MPSGraphTensor *)x {
    if (_isAneModel) return [self nafblockNamedANE:prefix C:C input:x];
    NAFBlockNames n = {
        .gamma1 = [NSString stringWithFormat:@"%@_norm1_weight", prefix],
        .beta1  = [NSString stringWithFormat:@"%@_norm1_bias",   prefix],
        .W1     = [NSString stringWithFormat:@"%@_conv1_weight", prefix],
        .B1     = [NSString stringWithFormat:@"%@_conv1_bias",   prefix],
        .DWw    = [NSString stringWithFormat:@"%@_dw_weight",    prefix],
        .DWb    = [NSString stringWithFormat:@"%@_dw_bias",      prefix],
        .Pj1w   = [NSString stringWithFormat:@"%@_proj1_weight", prefix],
        .Pj1b   = [NSString stringWithFormat:@"%@_proj1_bias",   prefix],
        .gamma2 = [NSString stringWithFormat:@"%@_norm2_weight", prefix],
        .beta2  = [NSString stringWithFormat:@"%@_norm2_bias",   prefix],
        .W2     = [NSString stringWithFormat:@"%@_mlp1_weight",  prefix],
        .B2     = [NSString stringWithFormat:@"%@_mlp1_bias",    prefix],
        .Pj2w   = [NSString stringWithFormat:@"%@_mlp2_weight",  prefix],
        .Pj2b   = [NSString stringWithFormat:@"%@_mlp2_bias",    prefix],
    };
    return [self buildNAFBlock:x C:C names:n basename:prefix];
}

// F_ane variant: BN folded into Conv1x1, SiLU, proj1/mlp2 are [C, 2C].
- (MPSGraphTensor *)nafblockNamedANE:(NSString *)prefix C:(NSUInteger)C input:(MPSGraphTensor *)x {
    MPSGraph *g = _graph;
    NSUInteger k = (NSUInteger)_dwKernel;

    // Attention branch: conv1(c→2c) → dw(k×k) → SiLU → proj1(2c→c) → +x
    MPSGraphTensor *W1   = constTensorHWIO(g, _W.buffers[[prefix stringByAppendingString:@"_conv1_weight"]], 1, 1, C, 2*C);
    MPSGraphTensor *B1   = constTensor1D(g, _W.buffers[[prefix stringByAppendingString:@"_conv1_bias"]], 2*C);
    MPSGraphTensor *DWw  = constTensorDW_HWIO(g, _W.buffers[[prefix stringByAppendingString:@"_dw_weight"]], k, k, 2*C);
    MPSGraphTensor *DWb  = constTensor1D(g, _W.buffers[[prefix stringByAppendingString:@"_dw_bias"]], 2*C);
    MPSGraphTensor *Pj1w = constTensorHWIO(g, _W.buffers[[prefix stringByAppendingString:@"_proj1_weight"]], 1, 1, 2*C, C);
    MPSGraphTensor *Pj1b = constTensor1D(g, _W.buffers[[prefix stringByAppendingString:@"_proj1_bias"]], C);

    MPSGraphTensor *y = conv2d_NHWC(g, x, W1, B1, 1, 0, [prefix stringByAppendingString:@"_ane_conv1"]);
    y = dwconvK_NHWC(g, y, DWw, DWb, k, [prefix stringByAppendingString:@"_ane_dw"]);
    y = silu_NHWC(g, y, [prefix stringByAppendingString:@"_ane_silu1"]);
    y = conv2d_NHWC(g, y, Pj1w, Pj1b, 1, 0, [prefix stringByAppendingString:@"_ane_proj1"]);
    x = [g additionWithPrimaryTensor:x secondaryTensor:y name:[prefix stringByAppendingString:@"_ane_res1"]];

    // MLP branch: mlp1(c→2c) → SiLU → mlp2(2c→c) → +x
    MPSGraphTensor *W2   = constTensorHWIO(g, _W.buffers[[prefix stringByAppendingString:@"_mlp1_weight"]], 1, 1, C, 2*C);
    MPSGraphTensor *B2   = constTensor1D(g, _W.buffers[[prefix stringByAppendingString:@"_mlp1_bias"]], 2*C);
    MPSGraphTensor *Pj2w = constTensorHWIO(g, _W.buffers[[prefix stringByAppendingString:@"_mlp2_weight"]], 1, 1, 2*C, C);
    MPSGraphTensor *Pj2b = constTensor1D(g, _W.buffers[[prefix stringByAppendingString:@"_mlp2_bias"]], C);

    y = conv2d_NHWC(g, x, W2, B2, 1, 0, [prefix stringByAppendingString:@"_ane_mlp1"]);
    y = silu_NHWC(g, y, [prefix stringByAppendingString:@"_ane_silu2"]);
    y = conv2d_NHWC(g, y, Pj2w, Pj2b, 1, 0, [prefix stringByAppendingString:@"_ane_mlp2"]);
    return [g additionWithPrimaryTensor:x secondaryTensor:y name:[prefix stringByAppendingString:@"_ane_res2"]];
}

// Parse MANIFEST.txt: sets _isAneModel and _dwKernel.
// Legacy F weights have no manifest → _isAneModel=NO, _dwKernel=3.
// _modelWidth is detected from the intro_weight.bin file size regardless of
// manifest presence — works for both legacy F and F_ane checkpoints.
- (void)parseManifest:(NSString *)dir {
    _isAneModel = NO;
    _dwKernel = 3;
    _modelWidth = 16;   // default, overridden by intro-size detection below
    NSString *manifestPath = [dir stringByAppendingPathComponent:@"MANIFEST.txt"];
    NSError *err = nil;
    NSString *contents = [NSString stringWithContentsOfFile:manifestPath
                                                    encoding:NSUTF8StringEncoding error:&err];
    if (contents) {
        for (NSString *raw in [contents componentsSeparatedByString:@"\n"]) {
            NSString *line = [raw stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
            if ([line hasPrefix:@"bn_folded=true"]) _isAneModel = YES;
            if ([line hasPrefix:@"dw_kernel="]) {
                _dwKernel = (uint32_t)[[line substringFromIndex:10] intValue];
            }
            if ([line hasPrefix:@"width="]) {
                _modelWidth = (uint32_t)[[line substringFromIndex:6] intValue];
            }
        }
    }
    /* Cross-check (and source-of-truth for legacy weights with no manifest):
       intro_weight.bin layout is (Cout=width, Cin=4, kH=3, kW=3) fp16 →
       width * 4 * 9 * 2 bytes. */
    NSString *introPath = [dir stringByAppendingPathComponent:@"intro_weight.bin"];
    NSError *attrErr = nil;
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfItemAtPath:introPath
                                                                            error:&attrErr];
    if (attrs) {
        unsigned long long sz = [attrs[NSFileSize] unsignedLongLongValue];
        uint32_t detected = (uint32_t)(sz / 72);   // 4*3*3*2 = 72 bytes per output channel
        if (detected >= 8 && detected <= 128) {
            if (contents && _modelWidth != detected) {
                fprintf(stderr, "SuperResMetal: manifest width=%u disagrees with intro_weight.bin (%u); using intro\n",
                        _modelWidth, detected);
            }
            _modelWidth = detected;
        }
    }
    if (_isAneModel) {
        fprintf(stderr, "SuperResMetal: F_ane backend detected (BN-folded, dw=%ux%u, width=%u)\n",
                _dwKernel, _dwKernel, _modelWidth);
    } else {
        fprintf(stderr, "SuperResMetal: legacy F backend (width=%u)\n", _modelWidth);
    }
}

// ---------- Weight load ----------
- (BOOL)loadWeights:(NSString *)dir {
    [self parseManifest:dir];
    if (_isAneModel) return [self loadWeightsANE:dir];

    SuperResMetalWeights *W = [[SuperResMetalWeights alloc] initWithDevice:_device];
    const uint32_t w = _modelWidth;

    // intro: Conv3x3 4 -> width
    NSString *introW = [dir stringByAppendingPathComponent:@"intro_weight.bin"];
    NSString *introB = [dir stringByAppendingPathComponent:@"intro_bias.bin"];
    if (![W loadConv4D:@"intro_weight" fromPath:introW Cout:w Cin:4 kH:3 kW:3]) return NO;
    if (![W loadRaw:@"intro_bias" fromPath:introB]) return NO;

    // encoders
    BOOL keepRaw = (_backend == SuperResMetalBackendHybrid);
    uint32_t enc_C[3] = {w, 2*w, 4*w};
    for (int k = 0; k < 3; k++) {
        uint32_t C = enc_C[k];
        NSString *p = [NSString stringWithFormat:@"enc%d", k];
        if (![W loadRaw:[p stringByAppendingString:@"_norm1_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm1_weight.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm1_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_conv1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                              Cout:2*C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_conv1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                          Cout:2*C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_conv1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadDWBoth:[p stringByAppendingString:@"_dw_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                          Cout:2*C kH:3 kW:3]) return NO;
        } else {
            if (![W loadDW:[p stringByAppendingString:@"_dw_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                      Cout:2*C kH:3 kW:3]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_dw_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_proj1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                              Cout:C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_proj1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                          Cout:C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_proj1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_bias.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm2_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm2_weight.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm2_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_mlp1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                              Cout:2*C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_mlp1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                          Cout:2*C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_mlp1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_mlp2_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                              Cout:C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_mlp2_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                          Cout:C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_mlp2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_bias.bin"]]]) return NO;
    }

    // downs: Conv 2x2 stride 2 (width, 2w, 4w → 2w, 4w, 8w)
    uint32_t down_pairs[3][2] = {{w, 2*w}, {2*w, 4*w}, {4*w, 8*w}};
    for (int k = 0; k < 3; k++) {
        NSString *p = [NSString stringWithFormat:@"down%d", k];
        if (![W loadConv4D:[p stringByAppendingString:@"_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_weight.bin"]]
                      Cout:down_pairs[k][1] Cin:down_pairs[k][0] kH:2 kW:2]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_bias.bin"]]]) return NO;
    }

    // middle NAFBlock at C = 8 * width
    {
        uint32_t C = 8 * w;
        NSString *p = @"middle";
        if (![W loadRaw:[p stringByAppendingString:@"_norm1_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm1_weight.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm1_bias.bin"]]]) return NO;
        if (![W loadConv4D:[p stringByAppendingString:@"_conv1_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                      Cout:2*C Cin:C kH:1 kW:1]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_conv1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_bias.bin"]]]) return NO;
        if (![W loadDW:[p stringByAppendingString:@"_dw_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                  Cout:2*C kH:3 kW:3]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_dw_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_bias.bin"]]]) return NO;
        if (![W loadConv4D:[p stringByAppendingString:@"_proj1_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                      Cout:C Cin:C kH:1 kW:1]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_proj1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_bias.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm2_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm2_weight.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm2_bias.bin"]]]) return NO;
        if (![W loadConv4D:[p stringByAppendingString:@"_mlp1_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                      Cout:2*C Cin:C kH:1 kW:1]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_mlp1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_bias.bin"]]]) return NO;
        if (![W loadConv4D:[p stringByAppendingString:@"_mlp2_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                      Cout:C Cin:C kH:1 kW:1]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_mlp2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_bias.bin"]]]) return NO;
    }

    // ups: Conv 1x1 (c_in -> 2*c_in), no bias (8w, 4w, 2w → 16w, 8w, 4w)
    uint32_t up_widths[3] = {8*w, 4*w, 2*w};
    for (int k = 0; k < 3; k++) {
        uint32_t cin = up_widths[k];
        NSString *p = [NSString stringWithFormat:@"up%d", k];
        if (![W loadConv4D:[p stringByAppendingString:@"_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_weight.bin"]]
                      Cout:2*cin Cin:cin kH:1 kW:1]) return NO;
    }

    // decoders at C = 4w, 2w, w
    uint32_t dec_C[3] = {4*w, 2*w, w};
    for (int k = 0; k < 3; k++) {
        uint32_t C = dec_C[k];
        NSString *p = [NSString stringWithFormat:@"dec%d", k];
        if (![W loadRaw:[p stringByAppendingString:@"_norm1_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm1_weight.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm1_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_conv1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                              Cout:2*C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_conv1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                          Cout:2*C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_conv1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadDWBoth:[p stringByAppendingString:@"_dw_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                          Cout:2*C kH:3 kW:3]) return NO;
        } else {
            if (![W loadDW:[p stringByAppendingString:@"_dw_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                      Cout:2*C kH:3 kW:3]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_dw_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_proj1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                              Cout:C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_proj1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                          Cout:C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_proj1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_bias.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm2_weight"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm2_weight.bin"]]]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_norm2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_norm2_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_mlp1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                              Cout:2*C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_mlp1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                          Cout:2*C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_mlp1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_bias.bin"]]]) return NO;
        if (keepRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_mlp2_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                              Cout:C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_mlp2_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                          Cout:C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_mlp2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_bias.bin"]]]) return NO;
    }

    // Head: variant-dependent.
    //   _useSubpixelHead=YES (F)       → subpixel head (16, width, 3, 3) + PS(2)
    //   _useSubpixelHead=NO  (F_no_sr) → outro head    ( 4, width, 3, 3)  at 1× dims
    if (_useSubpixelHead) {
        NSString *subW = [dir stringByAppendingPathComponent:@"subpixel_weight.bin"];
        NSString *subB = [dir stringByAppendingPathComponent:@"subpixel_bias.bin"];
        if (![W loadConv4D:@"subpixel_weight" fromPath:subW Cout:16 Cin:w kH:3 kW:3]) return NO;
        if (![W loadRaw:@"subpixel_bias" fromPath:subB]) return NO;
    } else {
        NSString *outroW = [dir stringByAppendingPathComponent:@"outro_weight.bin"];
        NSString *outroB = [dir stringByAppendingPathComponent:@"outro_bias.bin"];
        if (![W loadConv4D:@"outro_weight" fromPath:outroW Cout:4 Cin:w kH:3 kW:3]) return NO;
        if (![W loadRaw:@"outro_bias" fromPath:outroB]) return NO;
    }

    _W = W;
    return YES;
}

// ---------- F_ane weight loader ----------
// F_ane: BN folded into following Conv1x1, SiLU instead of SimpleGate,
// proj1/mlp2 shape [C, 2C] (was [C, C] in legacy), dw kernel may be 3 or 7.
// No norm1/norm2 blobs. Same buffer-key naming as legacy for everything else.
- (BOOL)loadWeightsANE:(NSString *)dir {
    SuperResMetalWeights *W = [[SuperResMetalWeights alloc] initWithDevice:_device];
    BOOL keepRaw = (_backend == SuperResMetalBackendHybrid);
    const uint32_t k = _dwKernel;
    const uint32_t w = _modelWidth;

    // intro: Conv3x3 4 -> width
    if (![W loadConv4D:@"intro_weight"
              fromPath:[dir stringByAppendingPathComponent:@"intro_weight.bin"]
                  Cout:w Cin:4 kH:3 kW:3]) return NO;
    if (![W loadRaw:@"intro_bias"
          fromPath:[dir stringByAppendingPathComponent:@"intro_bias.bin"]]) return NO;

    // One block loader: handles enc/middle/dec uniformly.
    BOOL (^loadBlock)(NSString *, uint32_t, BOOL) = ^BOOL(NSString *p, uint32_t C, BOOL needRaw) {
        // conv1 (BN-folded): [2C, C]
        if (needRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_conv1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                              Cout:2*C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_conv1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_weight.bin"]]
                          Cout:2*C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_conv1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_conv1_bias.bin"]]]) return NO;
        // dw: [2C, k*k]
        if (needRaw) {
            if (![W loadDWBoth:[p stringByAppendingString:@"_dw_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                          Cout:2*C kH:k kW:k]) return NO;
        } else {
            if (![W loadDW:[p stringByAppendingString:@"_dw_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_weight.bin"]]
                      Cout:2*C kH:k kW:k]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_dw_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_dw_bias.bin"]]]) return NO;
        // proj1: [C, 2C]   (NB: was [C, C] in legacy)
        if (needRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_proj1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                              Cout:C Cin:2*C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_proj1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_weight.bin"]]
                          Cout:C Cin:2*C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_proj1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_proj1_bias.bin"]]]) return NO;
        // mlp1 (BN-folded): [2C, C]
        if (needRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_mlp1_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                              Cout:2*C Cin:C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_mlp1_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_weight.bin"]]
                          Cout:2*C Cin:C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_mlp1_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp1_bias.bin"]]]) return NO;
        // mlp2: [C, 2C]   (NB: was [C, C] in legacy)
        if (needRaw) {
            if (![W loadConv4DBoth:[p stringByAppendingString:@"_mlp2_weight"]
                          fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                              Cout:C Cin:2*C kH:1 kW:1]) return NO;
        } else {
            if (![W loadConv4D:[p stringByAppendingString:@"_mlp2_weight"]
                      fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_weight.bin"]]
                          Cout:C Cin:2*C kH:1 kW:1]) return NO;
        }
        if (![W loadRaw:[p stringByAppendingString:@"_mlp2_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_mlp2_bias.bin"]]]) return NO;
        return YES;
    };

    // Encoders + downs
    uint32_t enc_C[3] = {w, 2*w, 4*w};
    for (int k = 0; k < 3; k++) {
        if (!loadBlock([NSString stringWithFormat:@"enc%d", k], enc_C[k], keepRaw)) return NO;
    }
    uint32_t down_pairs[3][2] = {{w, 2*w}, {2*w, 4*w}, {4*w, 8*w}};
    for (int k = 0; k < 3; k++) {
        NSString *p = [NSString stringWithFormat:@"down%d", k];
        if (![W loadConv4D:[p stringByAppendingString:@"_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_weight.bin"]]
                      Cout:down_pairs[k][1] Cin:down_pairs[k][0] kH:2 kW:2]) return NO;
        if (![W loadRaw:[p stringByAppendingString:@"_bias"]
              fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_bias.bin"]]]) return NO;
    }

    // Middle at C = 8*width — always MPSGraph (Metal kernels v[128] array doesn't fit 2*8*width).
    if (!loadBlock(@"middle", 8*w, NO)) return NO;

    // Ups + decoders
    uint32_t up_Cin[3] = {8*w, 4*w, 2*w};
    for (int k = 0; k < 3; k++) {
        NSString *p = [NSString stringWithFormat:@"up%d", k];
        if (![W loadConv4D:[p stringByAppendingString:@"_weight"]
                  fromPath:[dir stringByAppendingPathComponent:[p stringByAppendingString:@"_weight.bin"]]
                      Cout:2*up_Cin[k] Cin:up_Cin[k] kH:1 kW:1]) return NO;
    }
    uint32_t dec_C[3] = {4*w, 2*w, w};
    for (int k = 0; k < 3; k++) {
        if (!loadBlock([NSString stringWithFormat:@"dec%d", k], dec_C[k], keepRaw)) return NO;
    }

    // Head (ANE)
    if (_useSubpixelHead) {
        /* subpixel: Conv3x3(width → 4*out_c), followed by PixelShuffle(2) → out_c. */
        if (![W loadConv4D:@"subpixel_weight"
                  fromPath:[dir stringByAppendingPathComponent:@"subpixel_weight.bin"]
                      Cout:16 Cin:w kH:3 kW:3]) return NO;
        if (![W loadRaw:@"subpixel_bias"
              fromPath:[dir stringByAppendingPathComponent:@"subpixel_bias.bin"]]) return NO;
    } else {
        /* outro: Conv3x3(width → 4). */
        if (![W loadConv4D:@"outro_weight"
                  fromPath:[dir stringByAppendingPathComponent:@"outro_weight.bin"]
                      Cout:4 Cin:w kH:3 kW:3]) return NO;
        if (![W loadRaw:@"outro_bias"
              fromPath:[dir stringByAppendingPathComponent:@"outro_bias.bin"]]) return NO;
    }

    _W = W;
    return YES;
}

// ---------- Graph construction ----------
- (void)buildGraph {
    MPSGraph *g = [[MPSGraph alloc] init];
    _graph = g;

    // Input placeholder: (1, Hp, Wp, 4) fp16
    _inputTensor = [g placeholderWithShape:@[@1, @(_Hp), @(_Wp), @4]
                                  dataType:MPSDataTypeFloat16 name:@"input"];

    const NSUInteger w = _modelWidth;

    // ---- intro Conv3x3 4 -> width ----
    MPSGraphTensor *introW = constTensorHWIO(g, _W.buffers[@"intro_weight"], 3, 3, 4, w);
    MPSGraphTensor *introB = constTensor1D(g, _W.buffers[@"intro_bias"], w);
    MPSGraphTensor *y = conv2d_NHWC(g, _inputTensor, introW, introB, 1, 1, @"intro");

    // ---- enc0 (C=w) + skip + down0 ----
    y = [self nafblockNamed:@"enc0" C:w input:y];
    MPSGraphTensor *skip0 = y;
    MPSGraphTensor *down0W = constTensorHWIO(g, _W.buffers[@"down0_weight"], 2, 2, w, 2*w);
    MPSGraphTensor *down0B = constTensor1D(g, _W.buffers[@"down0_bias"], 2*w);
    y = conv2d_NHWC(g, y, down0W, down0B, 2, 0, @"down0");

    // ---- enc1 (C=2w) + skip + down1 ----
    y = [self nafblockNamed:@"enc1" C:2*w input:y];
    MPSGraphTensor *skip1 = y;
    MPSGraphTensor *down1W = constTensorHWIO(g, _W.buffers[@"down1_weight"], 2, 2, 2*w, 4*w);
    MPSGraphTensor *down1B = constTensor1D(g, _W.buffers[@"down1_bias"], 4*w);
    y = conv2d_NHWC(g, y, down1W, down1B, 2, 0, @"down1");

    // ---- enc2 (C=4w) + skip + down2 ----
    y = [self nafblockNamed:@"enc2" C:4*w input:y];
    MPSGraphTensor *skip2 = y;
    MPSGraphTensor *down2W = constTensorHWIO(g, _W.buffers[@"down2_weight"], 2, 2, 4*w, 8*w);
    MPSGraphTensor *down2B = constTensor1D(g, _W.buffers[@"down2_bias"], 8*w);
    y = conv2d_NHWC(g, y, down2W, down2B, 2, 0, @"down2");

    // ---- middle (C=8w) ----
    y = [self nafblockNamed:@"middle" C:8*w input:y];

    // ---- up0 (c_in=8w -> 4w via PS(2)) ----
    MPSGraphTensor *up0W = constTensorHWIO(g, _W.buffers[@"up0_weight"], 1, 1, 8*w, 16*w);
    y = conv2d_NHWC(g, y, up0W, nil, 1, 0, @"up0_conv");
    // PixelShuffle(2): NHWC depthToSpace with axes width=2, height=1, depth=3.
    y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                usePixelShuffleOrder:YES name:@"up0_ps"];
    // skip add (skip2 at C=4w)
    y = [g additionWithPrimaryTensor:y secondaryTensor:skip2 name:@"skip2_add"];
    // dec0 (C=4w)
    y = [self nafblockNamed:@"dec0" C:4*w input:y];

    // ---- up1 (c_in=4w -> 2w via PS(2)) ----
    MPSGraphTensor *up1W = constTensorHWIO(g, _W.buffers[@"up1_weight"], 1, 1, 4*w, 8*w);
    y = conv2d_NHWC(g, y, up1W, nil, 1, 0, @"up1_conv");
    y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                usePixelShuffleOrder:YES name:@"up1_ps"];
    y = [g additionWithPrimaryTensor:y secondaryTensor:skip1 name:@"skip1_add"];
    y = [self nafblockNamed:@"dec1" C:2*w input:y];

    // ---- up2 (c_in=2w -> w via PS(2)) ----
    MPSGraphTensor *up2W = constTensorHWIO(g, _W.buffers[@"up2_weight"], 1, 1, 2*w, 4*w);
    y = conv2d_NHWC(g, y, up2W, nil, 1, 0, @"up2_conv");
    y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                usePixelShuffleOrder:YES name:@"up2_ps"];
    y = [g additionWithPrimaryTensor:y secondaryTensor:skip0 name:@"skip0_add"];
    y = [self nafblockNamed:@"dec2" C:w input:y];

    // ---- Head: variant-dependent ----
    if (_useSubpixelHead) {
        // F: Conv3x3 w->16, PixelShuffle(2) -> 4 channels at 2x.
        MPSGraphTensor *subW = constTensorHWIO(g, _W.buffers[@"subpixel_weight"], 3, 3, w, 16);
        MPSGraphTensor *subB = constTensor1D(g, _W.buffers[@"subpixel_bias"], 16);
        y = conv2d_NHWC(g, y, subW, subB, 1, 1, @"subpixel");
        y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                    usePixelShuffleOrder:YES name:@"subpixel_ps"];
        _residualTensor = y;  // (1, 2Hp, 2Wp, 4) fp16
    } else {
        // F_no_sr: Conv3x3 w->4 (outro). Output at same plane dims as input.
        MPSGraphTensor *outW_ = constTensorHWIO(g, _W.buffers[@"outro_weight"], 3, 3, w, 4);
        MPSGraphTensor *outB_ = constTensor1D(g, _W.buffers[@"outro_bias"], 4);
        y = conv2d_NHWC(g, y, outW_, outB_, 1, 1, @"outro");
        _residualTensor = y;  // (1, Hp, Wp, 4) fp16
    }
}

// ============================================================================
// Hybrid path: 7 sub-graphs + 6 in-line Metal NAFBlock dispatches.
// ============================================================================

// Builds a small MPSGraph for one "non-NAFBlock segment". The caller specifies
// what the segment does via the build block. Each segment takes one or two
// inputs and produces a single output.
//
// Encoded buffers (skip connections) are pumped through later graphs by simply
// adding them as additional placeholders + tensors in those later graphs.

- (void)buildHybridGraphs {
    // ----- G1: intro (input 4ch → 16ch at Hp×Wp) -----
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gIntro = g;
        _gIntroIn = [g placeholderWithShape:@[@1, @(_Hp), @(_Wp), @4]
                                   dataType:MPSDataTypeFloat16 name:@"input"];
        MPSGraphTensor *introW = constTensorHWIO(g, _W.buffers[@"intro_weight"], 3, 3, 4, 16);
        MPSGraphTensor *introB = constTensor1D(g, _W.buffers[@"intro_bias"], 16);
        _gIntroOut = conv2d_NHWC(g, _gIntroIn, introW, introB, 1, 1, @"intro");
    }

    const NSUInteger w = _modelWidth;

    // ----- G2: down0 (w ch Hp×Wp → 2w ch Hp/2×Wp/2) -----
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gDown0 = g;
        _gDown0In = [g placeholderWithShape:@[@1, @(_Hp), @(_Wp), @(w)]
                                   dataType:MPSDataTypeFloat16 name:@"enc0_out"];
        MPSGraphTensor *down0W = constTensorHWIO(g, _W.buffers[@"down0_weight"], 2, 2, w, 2*w);
        MPSGraphTensor *down0B = constTensor1D(g, _W.buffers[@"down0_bias"], 2*w);
        _gDown0Out = conv2d_NHWC(g, _gDown0In, down0W, down0B, 2, 0, @"down0");
    }

    // ----- G3: down1 (2w ch Hp/2×Wp/2 → 4w ch Hp/4×Wp/4) -----
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gDown1 = g;
        _gDown1In = [g placeholderWithShape:@[@1, @(_Hp / 2), @(_Wp / 2), @(2*w)]
                                   dataType:MPSDataTypeFloat16 name:@"enc1_out"];
        MPSGraphTensor *down1W = constTensorHWIO(g, _W.buffers[@"down1_weight"], 2, 2, 2*w, 4*w);
        MPSGraphTensor *down1B = constTensor1D(g, _W.buffers[@"down1_bias"], 4*w);
        _gDown1Out = conv2d_NHWC(g, _gDown1In, down1W, down1B, 2, 0, @"down1");
    }

    // ----- G4: down2 + middle NAFBlock C=8w + up0(PS) + skip2_add -----
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gMid = g;
        _gMidIn = [g placeholderWithShape:@[@1, @(_Hp / 4), @(_Wp / 4), @(4*w)]
                                 dataType:MPSDataTypeFloat16 name:@"enc2_out"];
        _gMidSkip2 = [g placeholderWithShape:@[@1, @(_Hp / 4), @(_Wp / 4), @(4*w)]
                                    dataType:MPSDataTypeFloat16 name:@"skip2"];
        MPSGraphTensor *down2W = constTensorHWIO(g, _W.buffers[@"down2_weight"], 2, 2, 4*w, 8*w);
        MPSGraphTensor *down2B = constTensor1D(g, _W.buffers[@"down2_bias"], 8*w);
        MPSGraphTensor *y = conv2d_NHWC(g, _gMidIn, down2W, down2B, 2, 0, @"down2");
        // middle NAFBlock at C=8w
        MPSGraph *savedGraph = _graph; _graph = g;
        y = [self nafblockNamed:@"middle" C:8*w input:y];
        _graph = savedGraph;
        // up0: conv 1x1 (8w→16w) then PS(2) → 4w ch at Hp/4×Wp/4
        MPSGraphTensor *up0W = constTensorHWIO(g, _W.buffers[@"up0_weight"], 1, 1, 8*w, 16*w);
        y = conv2d_NHWC(g, y, up0W, nil, 1, 0, @"up0_conv");
        y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                    usePixelShuffleOrder:YES name:@"up0_ps"];
        y = [g additionWithPrimaryTensor:y secondaryTensor:_gMidSkip2 name:@"skip2_add"];
        _gMidOutDec0In = y;
    }

    // ----- G5: up1 + skip1_add (4w ch Hp/4×Wp/4 → 2w ch Hp/2×Wp/2) -----
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gUp1 = g;
        _gUp1In = [g placeholderWithShape:@[@1, @(_Hp / 4), @(_Wp / 4), @(4*w)]
                                 dataType:MPSDataTypeFloat16 name:@"dec0_out"];
        _gUp1Skip1 = [g placeholderWithShape:@[@1, @(_Hp / 2), @(_Wp / 2), @(2*w)]
                                    dataType:MPSDataTypeFloat16 name:@"skip1"];
        MPSGraphTensor *up1W = constTensorHWIO(g, _W.buffers[@"up1_weight"], 1, 1, 4*w, 8*w);
        MPSGraphTensor *y = conv2d_NHWC(g, _gUp1In, up1W, nil, 1, 0, @"up1_conv");
        y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                    usePixelShuffleOrder:YES name:@"up1_ps"];
        y = [g additionWithPrimaryTensor:y secondaryTensor:_gUp1Skip1 name:@"skip1_add"];
        _gUp1Out = y;
    }

    // ----- G6: up2 + skip0_add (2w ch Hp/2×Wp/2 → w ch Hp×Wp) -----
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gUp2 = g;
        _gUp2In = [g placeholderWithShape:@[@1, @(_Hp / 2), @(_Wp / 2), @(2*w)]
                                 dataType:MPSDataTypeFloat16 name:@"dec1_out"];
        _gUp2Skip0 = [g placeholderWithShape:@[@1, @(_Hp), @(_Wp), @(w)]
                                    dataType:MPSDataTypeFloat16 name:@"skip0"];
        MPSGraphTensor *up2W = constTensorHWIO(g, _W.buffers[@"up2_weight"], 1, 1, 2*w, 4*w);
        MPSGraphTensor *y = conv2d_NHWC(g, _gUp2In, up2W, nil, 1, 0, @"up2_conv");
        y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                    usePixelShuffleOrder:YES name:@"up2_ps"];
        y = [g additionWithPrimaryTensor:y secondaryTensor:_gUp2Skip0 name:@"skip0_add"];
        _gUp2Out = y;
    }

    // ----- G7: Head — variant-dependent
    //          F:       w ch Hp×Wp → 4ch 2Hp×2Wp (subpixel + PS)
    //          F_no_sr: w ch Hp×Wp → 4ch Hp×Wp   (outro Conv3x3 w→4)
    {
        MPSGraph *g = [[MPSGraph alloc] init];
        _gHead = g;
        _gHeadIn = [g placeholderWithShape:@[@1, @(_Hp), @(_Wp), @(w)]
                                  dataType:MPSDataTypeFloat16 name:@"dec2_out"];
        MPSGraphTensor *y;
        if (_useSubpixelHead) {
            MPSGraphTensor *subW = constTensorHWIO(g, _W.buffers[@"subpixel_weight"], 3, 3, w, 16);
            MPSGraphTensor *subB = constTensor1D(g, _W.buffers[@"subpixel_bias"], 16);
            y = conv2d_NHWC(g, _gHeadIn, subW, subB, 1, 1, @"subpixel");
            y = [g depthToSpace2DTensor:y widthAxis:2 heightAxis:1 depthAxis:3 blockSize:2
                        usePixelShuffleOrder:YES name:@"subpixel_ps"];
        } else {
            MPSGraphTensor *outW_ = constTensorHWIO(g, _W.buffers[@"outro_weight"], 3, 3, w, 4);
            MPSGraphTensor *outB_ = constTensor1D(g, _W.buffers[@"outro_bias"], 4);
            y = conv2d_NHWC(g, _gHeadIn, outW_, outB_, 1, 1, @"outro");
        }
        _gHeadOut = y;
    }
}

// ---------- Hybrid: PSO + intermediate buffer setup ----------

// Helper to build a PSO for a kernel with function-constant specialization.
static id<MTLComputePipelineState> makeNAFPSO5(id<MTLDevice> dev, id<MTLLibrary> lib,
                                               NSString *fname,
                                               uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3,
                                               uint32_t c4, int nconst)
{
    MTLFunctionConstantValues *fc = [[MTLFunctionConstantValues alloc] init];
    [fc setConstantValue:&c0 type:MTLDataTypeUInt atIndex:0];
    [fc setConstantValue:&c1 type:MTLDataTypeUInt atIndex:1];
    [fc setConstantValue:&c2 type:MTLDataTypeUInt atIndex:2];
    if (nconst >= 4) [fc setConstantValue:&c3 type:MTLDataTypeUInt atIndex:3];
    if (nconst >= 5) [fc setConstantValue:&c4 type:MTLDataTypeUInt atIndex:4];
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:fname constantValues:fc error:&err];
    if (!fn) {
        fprintf(stderr, "SuperResMetal: can't load %s: %s\n",
                [fname UTF8String], [[err localizedDescription] UTF8String]);
        return nil;
    }
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) {
        fprintf(stderr, "SuperResMetal: can't make pso %s: %s\n",
                [fname UTF8String], [[err localizedDescription] UTF8String]);
        return nil;
    }
    return pso;
}

static id<MTLComputePipelineState> makeNAFPSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                              NSString *fname,
                                              uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3,
                                              int nconst)
{
    MTLFunctionConstantValues *fc = [[MTLFunctionConstantValues alloc] init];
    [fc setConstantValue:&c0 type:MTLDataTypeUInt atIndex:0];
    [fc setConstantValue:&c1 type:MTLDataTypeUInt atIndex:1];
    [fc setConstantValue:&c2 type:MTLDataTypeUInt atIndex:2];
    if (nconst >= 4) [fc setConstantValue:&c3 type:MTLDataTypeUInt atIndex:3];
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:fname constantValues:fc error:&err];
    if (!fn) {
        fprintf(stderr, "SuperResMetal: can't load %s: %s\n",
                [fname UTF8String], [[err localizedDescription] UTF8String]);
        return nil;
    }
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) {
        fprintf(stderr, "SuperResMetal: can't make pso %s: %s\n",
                [fname UTF8String], [[err localizedDescription] UTF8String]);
        return nil;
    }
    return pso;
}

- (BOOL)setupHybridResources:(id<MTLLibrary>)lib {
    /* The Metal-hybrid kernels use a thread-local float v[128] register array
       to hold the 2C-channel intermediate. That caps 2C at 128, i.e. C ≤ 64,
       i.e. _modelWidth ≤ 16. For wider models the hybrid backend can't safely
       run — bail out and force MPSGraph instead. The caller (initWith… switch)
       handles the fallback. */
    if (_modelWidth > 16) {
        fprintf(stderr, "SuperResMetal: width=%u > 16 — hybrid kernels unsupported; "
                        "force MPSGraph backend\n", _modelWidth);
        return NO;
    }
    const uint32_t w = _modelWidth;
    // Spatial dims per level
    _levelH[0] = _Hp;       _levelW[0] = _Wp;
    _levelH[1] = _Hp / 2;   _levelW[1] = _Wp / 2;
    _levelH[2] = _Hp / 4;   _levelW[2] = _Wp / 4;
    uint32_t levelC[3] = {w, 2*w, 4*w};

    for (int lv = 0; lv < 3; lv++) {
        uint32_t C = levelC[lv], H = _levelH[lv], W = _levelW[lv];
        if (_isAneModel) {
            // F_ane PSOs.
            //   fused_ane_conv1x1: (C_IN=C, C_OUT=2C, W_DIM, H_DIM)
            _psoAneConv1x1[lv] = makeNAFPSO(_device, lib, @"fused_ane_conv1x1", C, 2*C, W, H, 4);
            if (!_psoAneConv1x1[lv]) return NO;
            //   fused_ane_dw_silu_proj_res: (C_PJ_IN=2C, C_OUT=C, W, H, DW_K)
            _psoAneDSPR[lv] = makeNAFPSO5(_device, lib, @"fused_ane_dw_silu_proj_res",
                                           2*C, C, W, H, _dwKernel, 5);
            if (!_psoAneDSPR[lv]) return NO;
            //   fused_ane_silu_proj_res: (C_PJ_IN=2C, C_OUT=C, W, H)
            _psoAneSPR[lv] = makeNAFPSO(_device, lib, @"fused_ane_silu_proj_res", 2*C, C, W, H, 4);
            if (!_psoAneSPR[lv]) return NO;
        } else {
            // Legacy F (LN+SG) PSOs.
            _psoLN[lv] = makeNAFPSO(_device, lib, @"fused_ln_conv1x1", C, 2*C, W, H, 4);
            if (!_psoLN[lv]) return NO;
            _psoDGR[lv] = makeNAFPSO(_device, lib, @"fused_dw_gate_proj_res", C, W, H, 0, 3);
            if (!_psoDGR[lv]) return NO;
            _psoGPR[lv] = makeNAFPSO(_device, lib, @"fused_gate_proj_res", C, W, H, 0, 3);
            if (!_psoGPR[lv]) return NO;
        }
    }

    // Intermediate buffers between graph segments and NAFBlocks.
    size_t b_l0 = (size_t)_levelH[0] * _levelW[0] * w     * sizeof(uint16_t);  // w ch full
    size_t b_l1 = (size_t)_levelH[1] * _levelW[1] * 2 * w * sizeof(uint16_t);  // 2w ch /2
    size_t b_l2 = (size_t)_levelH[2] * _levelW[2] * 4 * w * sizeof(uint16_t);  // 4w ch /4
    _bEnc0In  = [_device newBufferWithLength:b_l0 options:MTLResourceStorageModeShared];
    _bEnc0Out = [_device newBufferWithLength:b_l0 options:MTLResourceStorageModeShared];
    _bEnc1In  = [_device newBufferWithLength:b_l1 options:MTLResourceStorageModeShared];
    _bEnc1Out = [_device newBufferWithLength:b_l1 options:MTLResourceStorageModeShared];
    _bEnc2In  = [_device newBufferWithLength:b_l2 options:MTLResourceStorageModeShared];
    _bEnc2Out = [_device newBufferWithLength:b_l2 options:MTLResourceStorageModeShared];
    _bDec0In  = [_device newBufferWithLength:b_l2 options:MTLResourceStorageModeShared];
    _bDec0Out = [_device newBufferWithLength:b_l2 options:MTLResourceStorageModeShared];
    _bDec1In  = [_device newBufferWithLength:b_l1 options:MTLResourceStorageModeShared];
    _bDec1Out = [_device newBufferWithLength:b_l1 options:MTLResourceStorageModeShared];
    _bDec2In  = [_device newBufferWithLength:b_l0 options:MTLResourceStorageModeShared];
    _bDec2Out = [_device newBufferWithLength:b_l0 options:MTLResourceStorageModeShared];

    // Per-NAFBlock scratch z1 (2C HW), m (C HW), z2 (2C HW).
    // Indexing: 0=enc0, 1=enc1, 2=enc2, 3=dec0, 4=dec1, 5=dec2.
    uint32_t blockC[6] = {w, 2*w, 4*w, 4*w, 2*w, w};
    uint32_t blockH[6] = {_levelH[0], _levelH[1], _levelH[2], _levelH[2], _levelH[1], _levelH[0]};
    uint32_t blockW[6] = {_levelW[0], _levelW[1], _levelW[2], _levelW[2], _levelW[1], _levelW[0]};
    for (int i = 0; i < 6; i++) {
        size_t hw = (size_t)blockH[i] * blockW[i];
        _scratchZ1[i] = [_device newBufferWithLength:hw * 2 * blockC[i] * sizeof(uint16_t)
                                              options:MTLResourceStorageModePrivate];
        _scratchM[i]  = [_device newBufferWithLength:hw     * blockC[i] * sizeof(uint16_t)
                                              options:MTLResourceStorageModePrivate];
        _scratchZ2[i] = [_device newBufferWithLength:hw * 2 * blockC[i] * sizeof(uint16_t)
                                              options:MTLResourceStorageModePrivate];
    }
    return YES;
}

// Encode one NAFBlock at level `lv` (0=16, 1=32, 2=64).
//   inBuf  : [H, W, C] block input (also used as residual)
//   outBuf : [H, W, C] block output
//   scratchIdx : 0..5 picks _scratchZ1/M/Z2 slot
//   prefix : e.g. "enc0" — used for weight buffer lookup
- (void)encodeNAFBlock:(id<MTLCommandBuffer>)cb
                  level:(int)lv
              scratchIdx:(int)si
               prefix:(NSString *)prefix
                inBuf:(id<MTLBuffer>)inBuf
               outBuf:(id<MTLBuffer>)outBuf
{
    if (_isAneModel) {
        [self encodeNAFBlockANE:cb level:lv scratchIdx:si prefix:prefix
                          inBuf:inBuf outBuf:outBuf];
        return;
    }
    uint32_t W = _levelW[lv], H = _levelH[lv];
    MTLSize tg = MTLSizeMake(32, 8, 1);
    MTLSize grid = MTLSizeMake(W, H, 1);
    SuperResMetalWeights *Wb = _W;

    id<MTLBuffer> z1 = _scratchZ1[si];
    id<MTLBuffer> m  = _scratchM[si];
    id<MTLBuffer> z2 = _scratchZ2[si];

    id<MTLBuffer> g1 = Wb.buffers[[prefix stringByAppendingString:@"_norm1_weight"]];
    id<MTLBuffer> b1 = Wb.buffers[[prefix stringByAppendingString:@"_norm1_bias"]];
    id<MTLBuffer> W1 = Wb.buffers[[prefix stringByAppendingString:@"_conv1_weight_raw"]];
    id<MTLBuffer> B1 = Wb.buffers[[prefix stringByAppendingString:@"_conv1_bias"]];
    id<MTLBuffer> DW = Wb.buffers[[prefix stringByAppendingString:@"_dw_weight_raw"]];
    id<MTLBuffer> DB = Wb.buffers[[prefix stringByAppendingString:@"_dw_bias"]];
    id<MTLBuffer> P1W = Wb.buffers[[prefix stringByAppendingString:@"_proj1_weight_raw"]];
    id<MTLBuffer> P1B = Wb.buffers[[prefix stringByAppendingString:@"_proj1_bias"]];
    id<MTLBuffer> g2b = Wb.buffers[[prefix stringByAppendingString:@"_norm2_weight"]];
    id<MTLBuffer> b2b = Wb.buffers[[prefix stringByAppendingString:@"_norm2_bias"]];
    id<MTLBuffer> W2 = Wb.buffers[[prefix stringByAppendingString:@"_mlp1_weight_raw"]];
    id<MTLBuffer> B2 = Wb.buffers[[prefix stringByAppendingString:@"_mlp1_bias"]];
    id<MTLBuffer> P2W = Wb.buffers[[prefix stringByAppendingString:@"_mlp2_weight_raw"]];
    id<MTLBuffer> P2B = Wb.buffers[[prefix stringByAppendingString:@"_mlp2_bias"]];

    // All 4 NAFBlock sub-kernels share ONE compute encoder. Serial dispatch
    // (the default) provides implicit memory barriers between dispatches, so
    // each sub-kernel sees the previous one's writes. Saves ~3 encoder
    // create/teardown cycles per NAFBlock — 6 blocks × 3 = 18 dispatches less.
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

    // Stage 1: LN1 + Conv1 (C -> 2C). Buffers: in, gamma, beta, W1, B1, out(z1).
    [enc setComputePipelineState:_psoLN[lv]];
    [enc setBuffer:inBuf offset:0 atIndex:0];
    [enc setBuffer:g1    offset:0 atIndex:1];
    [enc setBuffer:b1    offset:0 atIndex:2];
    [enc setBuffer:W1    offset:0 atIndex:3];
    [enc setBuffer:B1    offset:0 atIndex:4];
    [enc setBuffer:z1    offset:0 atIndex:5];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    // Stage 2: DW + Gate + Proj + Residual. Buffers: z1, xres(inBuf), dw_w, dw_b, pj_w, pj_b, m.
    [enc setComputePipelineState:_psoDGR[lv]];
    [enc setBuffer:z1    offset:0 atIndex:0];
    [enc setBuffer:inBuf offset:0 atIndex:1];
    [enc setBuffer:DW    offset:0 atIndex:2];
    [enc setBuffer:DB    offset:0 atIndex:3];
    [enc setBuffer:P1W   offset:0 atIndex:4];
    [enc setBuffer:P1B   offset:0 atIndex:5];
    [enc setBuffer:m     offset:0 atIndex:6];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    // Stage 3: LN2 + mlp1 (C -> 2C). Buffers: m, gamma2, beta2, W2, B2, out(z2).
    [enc setComputePipelineState:_psoLN[lv]];
    [enc setBuffer:m     offset:0 atIndex:0];
    [enc setBuffer:g2b   offset:0 atIndex:1];
    [enc setBuffer:b2b   offset:0 atIndex:2];
    [enc setBuffer:W2    offset:0 atIndex:3];
    [enc setBuffer:B2    offset:0 atIndex:4];
    [enc setBuffer:z2    offset:0 atIndex:5];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    // Stage 4: Gate + Proj + Residual. Buffers: z2, xres(m), pj_w, pj_b, outBuf.
    [enc setComputePipelineState:_psoGPR[lv]];
    [enc setBuffer:z2    offset:0 atIndex:0];
    [enc setBuffer:m     offset:0 atIndex:1];
    [enc setBuffer:P2W   offset:0 atIndex:2];
    [enc setBuffer:P2B   offset:0 atIndex:3];
    [enc setBuffer:outBuf offset:0 atIndex:4];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    [enc endEncoding];
}

// F_ane variant of encodeNAFBlock. BN folded into Conv1x1, SiLU, dw_kernel k∈{3,7}.
- (void)encodeNAFBlockANE:(id<MTLCommandBuffer>)cb
                     level:(int)lv
                scratchIdx:(int)si
                    prefix:(NSString *)prefix
                     inBuf:(id<MTLBuffer>)inBuf
                    outBuf:(id<MTLBuffer>)outBuf
{
    uint32_t W = _levelW[lv], H = _levelH[lv];
    MTLSize tg = MTLSizeMake(32, 8, 1);
    MTLSize grid = MTLSizeMake(W, H, 1);
    SuperResMetalWeights *Wb = _W;

    id<MTLBuffer> z1 = _scratchZ1[si];
    id<MTLBuffer> m  = _scratchM[si];
    id<MTLBuffer> z2 = _scratchZ2[si];

    // No norm buffers — BN folded into conv1 and mlp1.
    id<MTLBuffer> W1  = Wb.buffers[[prefix stringByAppendingString:@"_conv1_weight_raw"]];
    id<MTLBuffer> B1  = Wb.buffers[[prefix stringByAppendingString:@"_conv1_bias"]];
    id<MTLBuffer> DW  = Wb.buffers[[prefix stringByAppendingString:@"_dw_weight_raw"]];
    id<MTLBuffer> DB  = Wb.buffers[[prefix stringByAppendingString:@"_dw_bias"]];
    id<MTLBuffer> P1W = Wb.buffers[[prefix stringByAppendingString:@"_proj1_weight_raw"]];
    id<MTLBuffer> P1B = Wb.buffers[[prefix stringByAppendingString:@"_proj1_bias"]];
    id<MTLBuffer> W2  = Wb.buffers[[prefix stringByAppendingString:@"_mlp1_weight_raw"]];
    id<MTLBuffer> B2  = Wb.buffers[[prefix stringByAppendingString:@"_mlp1_bias"]];
    id<MTLBuffer> P2W = Wb.buffers[[prefix stringByAppendingString:@"_mlp2_weight_raw"]];
    id<MTLBuffer> P2B = Wb.buffers[[prefix stringByAppendingString:@"_mlp2_bias"]];

    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];

    // Stage 1: BN1+Conv1 folded → Conv1x1 (C → 2C).
    //   buffers: in, wmat, bias, out(z1).
    [enc setComputePipelineState:_psoAneConv1x1[lv]];
    [enc setBuffer:inBuf offset:0 atIndex:0];
    [enc setBuffer:W1    offset:0 atIndex:1];
    [enc setBuffer:B1    offset:0 atIndex:2];
    [enc setBuffer:z1    offset:0 atIndex:3];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    // Stage 2: DW(k×k)+SiLU+proj1(2C→C)+Residual.
    //   buffers: in_2c(z1), dw_w, dw_b, pj_w, pj_b, res_in(inBuf), out(m).
    [enc setComputePipelineState:_psoAneDSPR[lv]];
    [enc setBuffer:z1    offset:0 atIndex:0];
    [enc setBuffer:DW    offset:0 atIndex:1];
    [enc setBuffer:DB    offset:0 atIndex:2];
    [enc setBuffer:P1W   offset:0 atIndex:3];
    [enc setBuffer:P1B   offset:0 atIndex:4];
    [enc setBuffer:inBuf offset:0 atIndex:5];
    [enc setBuffer:m     offset:0 atIndex:6];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    // Stage 3: BN2+mlp1 folded → Conv1x1 (C → 2C).
    [enc setComputePipelineState:_psoAneConv1x1[lv]];
    [enc setBuffer:m     offset:0 atIndex:0];
    [enc setBuffer:W2    offset:0 atIndex:1];
    [enc setBuffer:B2    offset:0 atIndex:2];
    [enc setBuffer:z2    offset:0 atIndex:3];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    // Stage 4: SiLU+mlp2(2C→C)+Residual.
    //   buffers: in_2c(z2), pj_w, pj_b, res_in(m), out(outBuf).
    [enc setComputePipelineState:_psoAneSPR[lv]];
    [enc setBuffer:z2     offset:0 atIndex:0];
    [enc setBuffer:P2W    offset:0 atIndex:1];
    [enc setBuffer:P2B    offset:0 atIndex:2];
    [enc setBuffer:m      offset:0 atIndex:3];
    [enc setBuffer:outBuf offset:0 atIndex:4];
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    [enc endEncoding];
}

// ---------- Init ----------

- (nullable instancetype)initWithWeightsDir:(NSString *)weightsDir
                                     device:(id<MTLDevice>)device
                                  inPlaneH:(uint32_t)inPlaneH
                                  inPlaneW:(uint32_t)inPlaneW
                                    backend:(SuperResMetalBackend)backend
{
    // Default: F variant (2× subpixel head) — backward-compatible.
    return [self initWithWeightsDir:weightsDir device:device
                           inPlaneH:inPlaneH inPlaneW:inPlaneW
                            backend:backend
                    useSubpixelHead:YES];
}

- (nullable instancetype)initWithWeightsDir:(NSString *)weightsDir
                                     device:(id<MTLDevice>)device
                                  inPlaneH:(uint32_t)inPlaneH
                                  inPlaneW:(uint32_t)inPlaneW
                                    backend:(SuperResMetalBackend)backend
                            useSubpixelHead:(BOOL)useSubpixelHead
{
    self = [super init];
    if (!self) return nil;
    _device = device;
    _queue = [device newCommandQueue];
    _mpsDevice = [MPSGraphDevice deviceWithMTLDevice:device];
    _Hp = inPlaneH;
    _Wp = inPlaneW;
    _backend = backend;
    _useSubpixelHead = useSubpixelHead;

    if (inPlaneH % 8 != 0 || inPlaneW % 8 != 0) {
        fprintf(stderr, "SuperResMetal: plane dims %ux%u not multiples of 8\n",
                inPlaneW, inPlaneH);
        return nil;
    }

    fprintf(stderr, "SuperResMetal: loading weights from %s ...\n", [weightsDir UTF8String]);
    double t0 = now_ms_local();
    if (![self loadWeights:weightsDir]) {
        fprintf(stderr, "SuperResMetal: load failed\n");
        return nil;
    }
    double t1 = now_ms_local();
    fprintf(stderr, "SuperResMetal: weights loaded in %.1f ms (%lu buffers)\n",
            t1 - t0, (unsigned long)_W.buffers.count);

    /* Hybrid requires width ≤ 16 (Metal kernels' v[128] register array).
       Quietly downgrade to MPSGraph before we build the wrong graph shape. */
    if (backend == SuperResMetalBackendHybrid && _modelWidth > 16) {
        fprintf(stderr, "SuperResMetal: width=%u > 16 — hybrid backend unsupported; "
                        "using MPSGraph\n", _modelWidth);
        _backend = SuperResMetalBackendMPSGraph;
        backend  = SuperResMetalBackendMPSGraph;
    }

    if (backend == SuperResMetalBackendHybrid) {
        [self buildHybridGraphs];
    } else {
        [self buildGraph];
    }
    double t2 = now_ms_local();
    fprintf(stderr, "SuperResMetal: graph built in %.1f ms (input %ux%u planes, backend=%ld)\n",
            t2 - t1, _Wp, _Hp, (long)_backend);

    // ---- Pre-compile MPSGraph to MPSGraphExecutable (MPSGraph backend only).
    // Compiling once at init lets MPSGraph specialize the kernel selection
    // ahead of time so per-frame encodeToCommandBuffer: is just an encode.
    // The plain MPSGraph encode path was observed to take ~34.5 ms / frame
    // (steady state); we expect this to drop with executable encode.
    // Opt out with GPR2PRORES_NO_EXEC_COMPILE=1 for A/B measurement.
    if (_backend == SuperResMetalBackendMPSGraph &&
        !getenv("GPR2PRORES_NO_EXEC_COMPILE")) {
        double tce0 = now_ms_local();
        MPSGraphCompilationDescriptor *cd =
            [[MPSGraphCompilationDescriptor alloc] init];
        cd.optimizationLevel = MPSGraphOptimizationLevel1;
        // Shape is fully static at this point (placeholder is fixed-shape) so
        // the executable should specialize without runtime shape inference.
        MPSGraphShapedType *inType = [[MPSGraphShapedType alloc]
            initWithShape:_inputTensor.shape
                 dataType:MPSDataTypeFloat16];
        _graphExec = [_graph compileWithDevice:_mpsDevice
                                         feeds:@{_inputTensor: inType}
                                  targetTensors:@[_residualTensor]
                               targetOperations:nil
                          compilationDescriptor:cd];
        if (!_graphExec) {
            fprintf(stderr, "SuperResMetal: compileWithDevice returned nil; "
                            "falling back to MPSGraph encode\n");
        } else {
            _graphExecDesc = [[MPSGraphExecutableExecutionDescriptor alloc] init];
            _graphExecDesc.waitUntilCompleted = NO;
            double tce1 = now_ms_local();
            fprintf(stderr, "SuperResMetal: graph compiled to executable in %.1f ms\n",
                    tce1 - tce0);
        }
    }

    // I/O buffers (fp16, NHWC) — also baseline buffer for the 2× fused path.
    //
    // 2× mode: planes (Hp, Wp, 4); residual & baseline at (2Hp, 2Wp, 4); output
    //          Bayer (4Hp × 4Wp) uint16 cells.
    // 1× mode: planes (Hp, Wp, 4); residual at (Hp, Wp, 4); output Bayer
    //          (2Hp × 2Wp) uint16 cells (same as input Bayer).
    size_t in_n  = (size_t)_Hp * _Wp * 4;
    size_t out_n = _useSubpixelHead
                       ? (size_t)(2 * _Hp) * (2 * _Wp) * 4
                       : (size_t)_Hp * _Wp * 4;
    _inBuf       = [_device newBufferWithLength:in_n  * sizeof(uint16_t) options:MTLResourceStorageModeShared];
    _outBuf      = [_device newBufferWithLength:out_n * sizeof(uint16_t) options:MTLResourceStorageModeShared];
    if (_useSubpixelHead) {
        _baselineBuf = [_device newBufferWithLength:out_n * sizeof(uint16_t)
                                            options:MTLResourceStorageModeShared];
    }
    // Zero the planes buffer ONCE so its padded region stays zero across calls.
    // The unpack kernel only writes the valid (Wp_in_native, Hp_in_native) region.
    memset(_inBuf.contents, 0, in_n * sizeof(uint16_t));

    // Bayer in/out buffers: sized generously.
    //   Input Bayer is (2*Hp × 2*Wp) uint16 (the unpack kernel only writes the
    //   valid sub-region; padding stays zero).
    //   Output Bayer dims depend on mode:
    //     2× mode: (4*Hp × 4*Wp) uint16 cells.
    //     1× mode: (2*Hp × 2*Wp) uint16 cells (== input dims).
    _inBayerCap  = (size_t)(2*_Hp) * (2*_Wp) * sizeof(uint16_t);
    _outBayerCap = _useSubpixelHead
                       ? (size_t)(4*_Hp) * (4*_Wp) * sizeof(uint16_t)
                       : (size_t)(2*_Hp) * (2*_Wp) * sizeof(uint16_t);
    _inBayer  = [_device newBufferWithLength:_inBayerCap  options:MTLResourceStorageModeShared];
    _outBayer = [_device newBufferWithLength:_outBayerCap options:MTLResourceStorageModeShared];

    // Load the post-processing compute pipelines from default.metallib.
    id<MTLLibrary> lib = [_device newDefaultLibrary];
    if (!lib) {
        fprintf(stderr, "SuperResMetal: cannot load default.metallib\n");
        return nil;
    }
    {
        id<MTLFunction> fn = [lib newFunctionWithName:@"unpack_bayer_to_nhwc4"];
        if (!fn) { fprintf(stderr, "SuperResMetal: missing kernel unpack_bayer_to_nhwc4\n"); return nil; }
        NSError *e = nil;
        _psoUnpack = [_device newComputePipelineStateWithFunction:fn error:&e];
        if (!_psoUnpack) { fprintf(stderr, "SuperResMetal: pso unpack: %s\n", [e.localizedDescription UTF8String]); return nil; }
    }
    {
        id<MTLFunction> fn = [lib newFunctionWithName:@"bicubic_2x_4chan"];
        if (!fn) { fprintf(stderr, "SuperResMetal: missing kernel bicubic_2x_4chan\n"); return nil; }
        NSError *e = nil;
        _psoBicubic = [_device newComputePipelineStateWithFunction:fn error:&e];
        if (!_psoBicubic) { fprintf(stderr, "SuperResMetal: pso bicubic: %s\n", [e.localizedDescription UTF8String]); return nil; }
    }
    {
        id<MTLFunction> fn = [lib newFunctionWithName:@"combine_rebayer"];
        if (!fn) { fprintf(stderr, "SuperResMetal: missing kernel combine_rebayer\n"); return nil; }
        NSError *e = nil;
        _psoCombine = [_device newComputePipelineStateWithFunction:fn error:&e];
        if (!_psoCombine) { fprintf(stderr, "SuperResMetal: pso combine: %s\n", [e.localizedDescription UTF8String]); return nil; }
    }
    {
        id<MTLFunction> fn = [lib newFunctionWithName:@"bicubic_combine_rebayer"];
        if (!fn) { fprintf(stderr, "SuperResMetal: missing kernel bicubic_combine_rebayer\n"); return nil; }
        NSError *e = nil;
        _psoBicubicCombine = [_device newComputePipelineStateWithFunction:fn error:&e];
        if (!_psoBicubicCombine) { fprintf(stderr, "SuperResMetal: pso bicubic_combine: %s\n", [e.localizedDescription UTF8String]); return nil; }
    }
    {
        id<MTLFunction> fn = [lib newFunctionWithName:@"combine_rebayer_1x"];
        if (!fn) { fprintf(stderr, "SuperResMetal: missing kernel combine_rebayer_1x\n"); return nil; }
        NSError *e = nil;
        _psoCombine1x = [_device newComputePipelineStateWithFunction:fn error:&e];
        if (!_psoCombine1x) { fprintf(stderr, "SuperResMetal: pso combine_rebayer_1x: %s\n", [e.localizedDescription UTF8String]); return nil; }
    }

    // Hybrid: build NAFBlock PSOs + intermediate buffers.
    if (_backend == SuperResMetalBackendHybrid) {
        if (![self setupHybridResources:lib]) {
            /* Hybrid requires width ≤ 16. For wider models we fall back
               cleanly to MPSGraph — the graphs have already been built
               above and are width-parameterized. */
            fprintf(stderr, "SuperResMetal: hybrid setup declined; falling back to MPSGraph\n");
            _backend = SuperResMetalBackendMPSGraph;
        }
    }

    // Warmup run (graph compile happens on first run).
    if (_backend == SuperResMetalBackendMPSGraph) {
        // Zero the input.
        memset(_inBuf.contents, 0, in_n * sizeof(uint16_t));
        MPSGraphTensorData *inData = [[MPSGraphTensorData alloc]
            initWithMTLBuffer:_inBuf
                        shape:@[@1, @(_Hp), @(_Wp), @4]
                     dataType:MPSDataTypeFloat16];
        NSArray<NSNumber *> *outShape = _useSubpixelHead
            ? @[@1, @(2*_Hp), @(2*_Wp), @4]
            : @[@1, @(_Hp),   @(_Wp),   @4];
        MPSGraphTensorData *outData = [[MPSGraphTensorData alloc]
            initWithMTLBuffer:_outBuf
                        shape:outShape
                     dataType:MPSDataTypeFloat16];
        double tw0 = now_ms_local();
        if (_graphExec) {
            [_graphExec runWithMTLCommandQueue:_queue
                                   inputsArray:@[inData]
                                  resultsArray:@[outData]
                            executionDescriptor:nil];
        } else {
            [_graph runWithMTLCommandQueue:_queue
                                     feeds:@{_inputTensor: inData}
                          targetOperations:nil
                         resultsDictionary:@{_residualTensor: outData}];
        }
        double tw1 = now_ms_local();
        fprintf(stderr, "SuperResMetal: warmup run %.1f ms (exec=%s)\n",
                tw1 - tw0, _graphExec ? "yes" : "no");
    } else {
        // Hybrid warmup: run the inference path once with zero input to
        // trigger graph compilation for all 7 sub-graphs + Metal kernels.
        memset(_inBuf.contents, 0, in_n * sizeof(uint16_t));
        // Use a small dummy bayer call by direct hybrid encoding.
        double tw0 = now_ms_local();
        uint16_t *zeroBayer = calloc(_inBayerCap / 2, sizeof(uint16_t));
        uint16_t *outBayer = calloc(_outBayerCap / 2, sizeof(uint16_t));
        uint32_t warmOutW = _useSubpixelHead ? (4*_Wp) : (2*_Wp);
        uint32_t warmOutH = _useSubpixelHead ? (4*_Hp) : (2*_Hp);
        [self runOnBayer:zeroBayer width:2*_Wp height:2*_Hp
                outBayer:outBayer outWidth:warmOutW outHeight:warmOutH
              blackLevel:0 whiteLevel:16383];
        free(zeroBayer); free(outBayer);
        double tw1 = now_ms_local();
        fprintf(stderr, "SuperResMetal: warmup run %.1f ms\n", tw1 - tw0);
    }

    return self;
}

// ---------- Inference ----------
// The bicubic baseline + combine + re-bayer + Bayer unpack are all GPU
// kernels now (see SuperResPost.metal). The MTLCommandBuffer carries:
//   unpack -> MPSGraph -> bicubic -> combine -> rebayer.

// Fused single-MTLCommandBuffer inference:
//   1) GPU unpack Bayer -> NHWC fp16 planes
//   2) MPSGraph residual
//   3) GPU bicubic baseline (NHWC)
//   4) GPU combine + clamp + re-bayer
//
// Implementation note: both runOnBayer:... outBayer: and the new zero-copy
// runOnBayer:... outMTLBuffer: dispatch into this same core. The destination
// MTLBuffer is either the persistent _outBayer (legacy CPU-copy path) or a
// caller-supplied IOSurface-backed buffer (zero-copy path). `outStridePix`
// selects how the rebayer kernel addresses rows.
- (int)_runCoreInBayer:(const uint16_t *)inBayer
                 inW:(uint32_t)inW inH:(uint32_t)inH
       destMTLBuffer:(id<MTLBuffer>)destMtlBuf
        destStridePix:(uint32_t)destStridePix
                outW:(uint32_t)outW outH:(uint32_t)outH
{
    int Hp_in_native = (int)inH / 2;
    int Wp_in_native = (int)inW / 2;
    if (Hp_in_native > (int)_Hp || Wp_in_native > (int)_Wp) {
        fprintf(stderr, "SuperResMetal: input planes %dx%d exceed graph %ux%u\n",
                Wp_in_native, Hp_in_native, _Wp, _Hp);
        return -1;
    }
    uint32_t Hpp = _Hp;
    uint32_t Wpp = _Wp;
    // In 1× mode the residual buffer is at plane dims; in 2× it's doubled.
    uint32_t out_Hpp = _useSubpixelHead ? (2u * Hpp) : Hpp;
    uint32_t out_Wpp = _useSubpixelHead ? (2u * Wpp) : Wpp;
    (void)Hp_in_native;

    double tp0 = now_ms_local();
    // Stage 0 (CPU): copy input Bayer into the persistent MTLBuffer.
    size_t inBayerBytes = (size_t)inW * inH * sizeof(uint16_t);
    if (inBayerBytes > _inBayerCap) {
        fprintf(stderr, "SuperResMetal: inBayer %zu > cap %zu\n", inBayerBytes, _inBayerCap);
        return -1;
    }
    // Parallel memcpy: 22.8 MB single-thread is ~0.9-1.5 ms; 4 GCD workers
    // bring it down to ~0.3 ms.
    {
        const size_t NCHUNKS = 4;
        const size_t chunk = (inBayerBytes + NCHUNKS - 1) / NCHUNKS;
        const uint8_t *src = (const uint8_t *)inBayer;
        uint8_t *dst = (uint8_t *)_inBayer.contents;
        dispatch_apply(NCHUNKS, DISPATCH_APPLY_AUTO, ^(size_t i){
            size_t off = i * chunk;
            size_t n = (off + chunk > inBayerBytes) ? (inBayerBytes - off) : chunk;
            if (n > 0) memcpy(dst + off, src + off, n);
        });
    }
    // NOTE: the planes buffer's padded region (>=Wp_in_native, >=Hp_in_native)
    // is zeroed ONCE at init / warmup time. The unpack kernel only writes the
    // valid region (Wp_in_native × Hp_in_native), so the padding stays zero
    // across calls — no per-frame memset needed.
    double tp1 = now_ms_local();

    // Optional per-stage profiling. SUPERRES_PROFILE=1 inserts commit+wait
    // between every encode and prints GPU time per stage. Breaks pipelining.
    static int profile_stages = -1;
    if (profile_stages < 0) {
        const char *e = getenv("SUPERRES_PROFILE");
        profile_stages = (e && e[0] == '1') ? 1 : 0;
    }
    static int profile_frame = 0;
    profile_frame++;
    // Skip first 3 frames (warmup); only print frame 4.
    int do_profile = (profile_stages && profile_frame == 4);

    // Build a fused MPSCommandBuffer (wraps an MTLCommandBuffer).
    MPSCommandBuffer *cb = [MPSCommandBuffer commandBufferFromCommandQueue:_queue];
    id<MTLCommandBuffer> rawCb = cb.commandBuffer;

    // Helper that commits the current MPSCommandBuffer, waits, prints GPU
    // time, then makes a fresh one. Used only when do_profile.
    #define PROFILE_STAGE(_label) do {                                         \
        if (do_profile) {                                                      \
            [cb commit]; [cb waitUntilCompleted];                              \
            double gpu_ms = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;        \
            fprintf(stderr, "    [profile] %-20s %6.2f ms GPU\n", _label, gpu_ms); \
            cb = [MPSCommandBuffer commandBufferFromCommandQueue:_queue];      \
            rawCb = cb.commandBuffer;                                          \
        }                                                                      \
    } while (0)

    // -- Stage A: GPU unpack Bayer -> NHWC fp16 planes (only valid region) --
    {
        id<MTLComputeCommandEncoder> enc = [rawCb computeCommandEncoder];
        [enc setComputePipelineState:_psoUnpack];
        [enc setBuffer:_inBayer offset:0 atIndex:0];
        [enc setBuffer:_inBuf   offset:0 atIndex:1];
        uint32_t inW_u = inW;
        uint32_t Wpp_u = Wpp;
        uint32_t Wp_u  = (uint32_t)Wp_in_native;
        uint32_t Hp_u  = (uint32_t)Hp_in_native;
        [enc setBytes:&inW_u length:sizeof(uint32_t) atIndex:2];
        [enc setBytes:&Wpp_u length:sizeof(uint32_t) atIndex:3];
        [enc setBytes:&Wp_u  length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&Hp_u  length:sizeof(uint32_t) atIndex:5];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(Wp_u, Hp_u, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
    }

    // -- Stage B: residual via MPSGraph (MPSGraph backend)
    //              or 7 sub-graphs + 6 NAFBlock kernels (Hybrid backend) --
    if (_backend == SuperResMetalBackendMPSGraph) {
        MPSGraphTensorData *inData = [[MPSGraphTensorData alloc]
            initWithMTLBuffer:_inBuf
                        shape:@[@1, @(_Hp), @(_Wp), @4]
                     dataType:MPSDataTypeFloat16];
        NSArray<NSNumber *> *outShape = _useSubpixelHead
            ? @[@1, @(2*_Hp), @(2*_Wp), @4]
            : @[@1, @(_Hp),   @(_Wp),   @4];
        MPSGraphTensorData *outData = [[MPSGraphTensorData alloc]
            initWithMTLBuffer:_outBuf
                        shape:outShape
                     dataType:MPSDataTypeFloat16];
        if (_graphExec) {
            [_graphExec encodeToCommandBuffer:cb
                                  inputsArray:@[inData]
                                 resultsArray:@[outData]
                          executionDescriptor:_graphExecDesc];
        } else {
            [_graph encodeToCommandBuffer:cb
                                    feeds:@{_inputTensor: inData}
                         targetOperations:nil
                        resultsDictionary:@{_residualTensor: outData}
                      executionDescriptor:nil];
        }
    } else {
        // ---- Hybrid: G1(intro) → enc0 → G2(down0) → enc1 → G3(down1) → enc2
        //              → G4(down2+mid+up0+skip2) → dec0 → G5(up1+skip1) → dec1
        //              → G6(up2+skip0) → dec2 → G7(SR head, writes _outBuf) ----
        MPSGraphTensorData *(^td)(id<MTLBuffer>, NSArray<NSNumber*>*) =
            ^(id<MTLBuffer> b, NSArray<NSNumber*> *shape) {
                return [[MPSGraphTensorData alloc] initWithMTLBuffer:b shape:shape
                                                            dataType:MPSDataTypeFloat16];
        };

        PROFILE_STAGE("unpack(A)");
        // G1: intro → _bEnc0In
        [_gIntro encodeToCommandBuffer:cb
                                 feeds:@{_gIntroIn: td(_inBuf,    @[@1, @(_Hp),     @(_Wp),     @4])}
                      targetOperations:nil
                     resultsDictionary:@{_gIntroOut: td(_bEnc0In, @[@1, @(_Hp),     @(_Wp),     @16])}
                   executionDescriptor:nil];
        PROFILE_STAGE("G1 intro");

        id<MTLCommandBuffer> rcb;
        // NAF enc0 (C=16) — full plane
        rcb = cb.commandBuffer;
        [self encodeNAFBlock:rcb level:0 scratchIdx:0 prefix:@"enc0"
                       inBuf:_bEnc0In outBuf:_bEnc0Out];
        PROFILE_STAGE("NAF enc0 C=16");

        // G2: down0 → _bEnc1In
        [_gDown0 encodeToCommandBuffer:cb
                                 feeds:@{_gDown0In: td(_bEnc0Out, @[@1, @(_Hp),     @(_Wp),     @16])}
                      targetOperations:nil
                     resultsDictionary:@{_gDown0Out: td(_bEnc1In, @[@1, @(_Hp/2),   @(_Wp/2),   @32])}
                   executionDescriptor:nil];
        PROFILE_STAGE("G2 down0");

        // NAF enc1 (C=32) — half plane
        rcb = cb.commandBuffer;
        [self encodeNAFBlock:rcb level:1 scratchIdx:1 prefix:@"enc1"
                       inBuf:_bEnc1In outBuf:_bEnc1Out];
        PROFILE_STAGE("NAF enc1 C=32");

        // G3: down1 → _bEnc2In
        [_gDown1 encodeToCommandBuffer:cb
                                 feeds:@{_gDown1In: td(_bEnc1Out, @[@1, @(_Hp/2),   @(_Wp/2),   @32])}
                      targetOperations:nil
                     resultsDictionary:@{_gDown1Out: td(_bEnc2In, @[@1, @(_Hp/4),   @(_Wp/4),   @64])}
                   executionDescriptor:nil];
        PROFILE_STAGE("G3 down1");

        // NAF enc2 (C=64) — quarter plane
        rcb = cb.commandBuffer;
        [self encodeNAFBlock:rcb level:2 scratchIdx:2 prefix:@"enc2"
                       inBuf:_bEnc2In outBuf:_bEnc2Out];
        PROFILE_STAGE("NAF enc2 C=64");

        // G4: down2 + middle(C=128) + up0 + skip2_add → _bDec0In
        [_gMid encodeToCommandBuffer:cb
                               feeds:@{_gMidIn:    td(_bEnc2Out, @[@1, @(_Hp/4),   @(_Wp/4),   @64]),
                                       _gMidSkip2: td(_bEnc2Out, @[@1, @(_Hp/4),   @(_Wp/4),   @64])}
                    targetOperations:nil
                   resultsDictionary:@{_gMidOutDec0In: td(_bDec0In, @[@1, @(_Hp/4), @(_Wp/4), @64])}
                 executionDescriptor:nil];
        PROFILE_STAGE("G4 mid (MPS C=128)");

        // NAF dec0 (C=64)
        rcb = cb.commandBuffer;
        [self encodeNAFBlock:rcb level:2 scratchIdx:3 prefix:@"dec0"
                       inBuf:_bDec0In outBuf:_bDec0Out];
        PROFILE_STAGE("NAF dec0 C=64");

        // G5: up1 + skip1_add → _bDec1In
        [_gUp1 encodeToCommandBuffer:cb
                               feeds:@{_gUp1In:    td(_bDec0Out, @[@1, @(_Hp/4), @(_Wp/4), @64]),
                                       _gUp1Skip1: td(_bEnc1Out, @[@1, @(_Hp/2), @(_Wp/2), @32])}
                    targetOperations:nil
                   resultsDictionary:@{_gUp1Out: td(_bDec1In, @[@1, @(_Hp/2), @(_Wp/2), @32])}
                 executionDescriptor:nil];
        PROFILE_STAGE("G5 up1");

        // NAF dec1 (C=32)
        rcb = cb.commandBuffer;
        [self encodeNAFBlock:rcb level:1 scratchIdx:4 prefix:@"dec1"
                       inBuf:_bDec1In outBuf:_bDec1Out];
        PROFILE_STAGE("NAF dec1 C=32");

        // G6: up2 + skip0_add → _bDec2In
        [_gUp2 encodeToCommandBuffer:cb
                               feeds:@{_gUp2In:    td(_bDec1Out, @[@1, @(_Hp/2), @(_Wp/2), @32]),
                                       _gUp2Skip0: td(_bEnc0Out, @[@1, @(_Hp),   @(_Wp),   @16])}
                    targetOperations:nil
                   resultsDictionary:@{_gUp2Out: td(_bDec2In, @[@1, @(_Hp), @(_Wp), @16])}
                 executionDescriptor:nil];
        PROFILE_STAGE("G6 up2");

        // NAF dec2 (C=16)
        rcb = cb.commandBuffer;
        [self encodeNAFBlock:rcb level:0 scratchIdx:5 prefix:@"dec2"
                       inBuf:_bDec2In outBuf:_bDec2Out];
        PROFILE_STAGE("NAF dec2 C=16");

        // G7: Head → _outBuf. Output shape depends on mode.
        NSArray<NSNumber *> *headOutShape = _useSubpixelHead
            ? @[@1, @(2*_Hp), @(2*_Wp), @4]
            : @[@1, @(_Hp),   @(_Wp),   @4];
        [_gHead encodeToCommandBuffer:cb
                                feeds:@{_gHeadIn: td(_bDec2Out, @[@1, @(_Hp), @(_Wp), @16])}
                     targetOperations:nil
                    resultsDictionary:@{_gHeadOut: td(_outBuf, headOutShape)}
                  executionDescriptor:nil];
        PROFILE_STAGE("G7 head");
    }

    // After MPSGraph encode, cb.commandBuffer may have changed (MPSCommandBuffer
    // can advance internally). Re-fetch it before appending kernels.
    id<MTLCommandBuffer> rawCb2 = cb.commandBuffer;

    // -- Stage C+D fused: post-processing into _outBayer.
    //   2× mode: bicubic + combine + clamp + rebayer (input planes + residual
    //            -> 4*Hp × 4*Wp output bayer).
    //   1× mode: combine + clamp + rebayer (input planes + residual at same
    //            plane dims -> 2*Hp × 2*Wp output bayer; no bicubic baseline).
    if (!_useSubpixelHead) {
        id<MTLComputeCommandEncoder> enc = [rawCb2 computeCommandEncoder];
        [enc setComputePipelineState:_psoCombine1x];
        [enc setBuffer:_inBuf    offset:0 atIndex:0];
        [enc setBuffer:_outBuf   offset:0 atIndex:1];
        [enc setBuffer:destMtlBuf offset:0 atIndex:2];
        uint32_t Wpp_u = Wpp, Hpp_u = Hpp;
        uint32_t outW_u = outW, outH_u = outH;
        uint32_t outStridePix_u = destStridePix;
        [enc setBytes:&Wpp_u  length:sizeof(uint32_t) atIndex:3];
        [enc setBytes:&Hpp_u  length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&outW_u length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&outH_u length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&outStridePix_u length:sizeof(uint32_t) atIndex:7];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(outW / 2, outH / 2, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
    } else if (getenv("SUPERRES_NOFUSE_POST")) {
        // Legacy two-kernel path (kept for A/B comparison).
        id<MTLComputeCommandEncoder> enc = [rawCb2 computeCommandEncoder];
        [enc setComputePipelineState:_psoBicubic];
        [enc setBuffer:_inBuf       offset:0 atIndex:0];
        [enc setBuffer:_baselineBuf offset:0 atIndex:1];
        uint32_t Wp_u = Wpp;
        uint32_t Hp_u = Hpp;
        [enc setBytes:&Wp_u length:sizeof(uint32_t) atIndex:2];
        [enc setBytes:&Hp_u length:sizeof(uint32_t) atIndex:3];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(out_Wpp, out_Hpp, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];

        enc = [rawCb2 computeCommandEncoder];
        [enc setComputePipelineState:_psoCombine];
        [enc setBuffer:_baselineBuf offset:0 atIndex:0];
        [enc setBuffer:_outBuf      offset:0 atIndex:1];
        [enc setBuffer:destMtlBuf   offset:0 atIndex:2];
        uint32_t outW_u = outW, outH_u = outH;
        uint32_t outWpp_u = out_Wpp, outHpp_u = out_Hpp;
        uint32_t outStridePix_u = destStridePix;
        [enc setBytes:&outWpp_u length:sizeof(uint32_t) atIndex:3];
        [enc setBytes:&outHpp_u length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&outW_u  length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&outH_u  length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&outStridePix_u length:sizeof(uint32_t) atIndex:7];
        MTLSize tg2 = MTLSizeMake(16, 16, 1);
        MTLSize grid2 = MTLSizeMake(outW / 2, outH / 2, 1);
        [enc dispatchThreads:grid2 threadsPerThreadgroup:tg2];
        [enc endEncoding];
    } else {
        id<MTLComputeCommandEncoder> enc = [rawCb2 computeCommandEncoder];
        [enc setComputePipelineState:_psoBicubicCombine];
        [enc setBuffer:_inBuf    offset:0 atIndex:0];
        [enc setBuffer:_outBuf   offset:0 atIndex:1];
        [enc setBuffer:destMtlBuf offset:0 atIndex:2];
        uint32_t Wpp_u = Wpp, Hpp_u = Hpp;
        uint32_t outWpp_u = out_Wpp, outHpp_u = out_Hpp;
        uint32_t outW_u = outW, outH_u = outH;
        uint32_t outStridePix_u = destStridePix;
        [enc setBytes:&Wpp_u    length:sizeof(uint32_t) atIndex:3];
        [enc setBytes:&Hpp_u    length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&outWpp_u length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&outHpp_u length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&outW_u   length:sizeof(uint32_t) atIndex:7];
        [enc setBytes:&outH_u   length:sizeof(uint32_t) atIndex:8];
        [enc setBytes:&outStridePix_u length:sizeof(uint32_t) atIndex:9];
        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(outW / 2, outH / 2, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    double tp2 = now_ms_local();

    static int _smcnt = 0;
    if (getenv("SUPERRES_METAL_TIMING")) {
        fprintf(stderr, "    SuperResMetal #%d  host=%.1f  gpu=%.1f\n",
                _smcnt++, tp1-tp0, tp2-tp1);
    }
    return 0;
}

// Legacy CPU-output entry point. Routes to the core then copies the persistent
// _outBayer into the caller's CPU buffer (with parallel memcpy across 4 GCD
// workers). Kept for callers (tests, no-CNN, etc.) that haven't moved to the
// IOSurface zero-copy path.
- (int)runOnBayer:(const uint16_t *)inBayer
            width:(uint32_t)inW height:(uint32_t)inH
         outBayer:(uint16_t *)outBayer
         outWidth:(uint32_t)outW
        outHeight:(uint32_t)outH
       blackLevel:(uint32_t)blackLevel
       whiteLevel:(uint32_t)whiteLevel
{
    (void)blackLevel; (void)whiteLevel;
    size_t outBayerBytes = (size_t)outW * outH * sizeof(uint16_t);
    if (outBayerBytes > _outBayerCap) {
        fprintf(stderr, "SuperResMetal: outBayer %zu > cap %zu\n", outBayerBytes, _outBayerCap);
        return -1;
    }
    int rc = [self _runCoreInBayer:inBayer inW:inW inH:inH
                      destMTLBuffer:_outBayer
                       destStridePix:outW
                              outW:outW outH:outH];
    if (rc != 0) return rc;

    // Copy output Bayer back to caller's CPU buffer. Parallel 4-way memcpy.
    double tp2 = now_ms_local();
    {
        const size_t NCHUNKS = 4;
        const size_t chunk = (outBayerBytes + NCHUNKS - 1) / NCHUNKS;
        const uint8_t *src = (const uint8_t *)_outBayer.contents;
        uint8_t *dst = (uint8_t *)outBayer;
        dispatch_apply(NCHUNKS, DISPATCH_APPLY_AUTO, ^(size_t i){
            size_t off = i * chunk;
            size_t n = (off + chunk > outBayerBytes) ? (outBayerBytes - off) : chunk;
            if (n > 0) memcpy(dst + off, src + off, n);
        });
    }
    double tp3 = now_ms_local();
    if (getenv("SUPERRES_METAL_TIMING")) {
        fprintf(stderr, "    SuperResMetal copyOut=%.1f\n", tp3 - tp2);
    }
    return 0;
}

// Zero-copy entry point: writes the final Bayer directly into the supplied
// MTLBuffer (typically IOSurface-backed). No CPU memcpy of the output Bayer.
- (int)runOnBayer:(const uint16_t *)inBayer
            width:(uint32_t)inW height:(uint32_t)inH
     outMTLBuffer:(id<MTLBuffer>)outMTLBuffer
   outStrideBytes:(size_t)outStrideBytes
         outWidth:(uint32_t)outW
        outHeight:(uint32_t)outH
       blackLevel:(uint32_t)blackLevel
       whiteLevel:(uint32_t)whiteLevel
{
    (void)blackLevel; (void)whiteLevel;
    if (!outMTLBuffer) {
        fprintf(stderr, "SuperResMetal: outMTLBuffer is nil\n");
        return -1;
    }
    if ((outStrideBytes & 1u) != 0) {
        fprintf(stderr, "SuperResMetal: outStrideBytes %zu must be even\n", outStrideBytes);
        return -1;
    }
    if (outStrideBytes < (size_t)outW * sizeof(uint16_t)) {
        fprintf(stderr, "SuperResMetal: outStrideBytes %zu < outW*2 %zu\n",
                outStrideBytes, (size_t)outW * sizeof(uint16_t));
        return -1;
    }
    size_t neededBytes = (size_t)outH * outStrideBytes;
    if (neededBytes > outMTLBuffer.length) {
        fprintf(stderr, "SuperResMetal: outMTLBuffer.length %zu < needed %zu\n",
                (size_t)outMTLBuffer.length, neededBytes);
        return -1;
    }
    uint32_t destStridePix = (uint32_t)(outStrideBytes / sizeof(uint16_t));
    return [self _runCoreInBayer:inBayer inW:inW inH:inH
                    destMTLBuffer:outMTLBuffer
                     destStridePix:destStridePix
                            outW:outW outH:outH];
}

@end
