// SuperResCNN.m — Phase-3 stub. We implement this in Phase 3.

#import "SuperResCNN.h"
#import <CoreML/CoreML.h>
#import <Accelerate/Accelerate.h>

@implementation SuperResCNN {
    MLModel *_model;
    NSString *_inputName;
    NSString *_outputName;
    NSArray<NSNumber *> *_inputShape;   // (1,4,Hp,Wp)
    NSArray<NSNumber *> *_outputShape;  // (1,4,2*Hp,2*Wp)
    id<MTLDevice> _device;
}

- (nullable instancetype)initWithMLPackagePath:(NSString *)path
                                         device:(id<MTLDevice>)device
{
    self = [super init];
    if (!self) return nil;
    _device = device;

    NSError *err = nil;
    NSURL *url = [NSURL fileURLWithPath:path];

    // CoreML requires a *compiled* model. mlpackages are uncompiled.
    NSURL *compiled = [MLModel compileModelAtURL:url error:&err];
    if (!compiled) {
        fprintf(stderr, "SuperResCNN: compile failed: %s\n",
                [err.localizedDescription UTF8String]);
        return nil;
    }

    MLModelConfiguration *cfg = [[MLModelConfiguration alloc] init];
    // ALL enables Apple Neural Engine (ANE). Override via env if a specific
    // unit is needed for A/B benchmarking.
    cfg.computeUnits = MLComputeUnitsAll;
    const char *cu_env = getenv("CNN_COREML_UNITS");
    if (cu_env) {
        if (!strcmp(cu_env, "cpu"))      cfg.computeUnits = MLComputeUnitsCPUOnly;
        else if (!strcmp(cu_env, "gpu")) cfg.computeUnits = MLComputeUnitsCPUAndGPU;
        else if (!strcmp(cu_env, "ane")) cfg.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
        else if (!strcmp(cu_env, "all")) cfg.computeUnits = MLComputeUnitsAll;
    }
    fprintf(stderr, "SuperResCNN: computeUnits = %ld (0=cpu 1=cpuGpu 2=all 3=cpuAne)\n",
            (long)cfg.computeUnits);
    _model = [MLModel modelWithContentsOfURL:compiled configuration:cfg error:&err];
    if (!_model) {
        fprintf(stderr, "SuperResCNN: load failed: %s\n",
                [err.localizedDescription UTF8String]);
        return nil;
    }

    MLModelDescription *desc = _model.modelDescription;
    NSArray *inputNames = desc.inputDescriptionsByName.allKeys;
    NSArray *outputNames = desc.outputDescriptionsByName.allKeys;
    if (inputNames.count == 0 || outputNames.count == 0) {
        fprintf(stderr, "SuperResCNN: no in/out descriptions\n");
        return nil;
    }
    _inputName = inputNames[0];
    _outputName = outputNames[0];
    MLFeatureDescription *inDesc = desc.inputDescriptionsByName[_inputName];
    MLFeatureDescription *outDesc = desc.outputDescriptionsByName[_outputName];
    _inputShape = inDesc.multiArrayConstraint.shape;
    _outputShape = outDesc.multiArrayConstraint.shape;
    fprintf(stderr, "SuperResCNN: loaded %s\n  input '%s' shape=%s\n  output '%s' shape=%s\n",
            [path UTF8String],
            [_inputName UTF8String], [[_inputShape description] UTF8String],
            [_outputName UTF8String], [[_outputShape description] UTF8String]);

    return self;
}

// Helpers
static inline uint32_t udiv_ceil(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

static void planes_from_bayer(const uint16_t *bayer, uint32_t W, uint32_t H,
                              float scale, float *planes /* (4, H/2, W/2) */) {
    uint32_t hp = H / 2, wp = W / 2;
    float *R  = planes + 0 * hp * wp;
    float *G1 = planes + 1 * hp * wp;
    float *G2 = planes + 2 * hp * wp;
    float *B  = planes + 3 * hp * wp;
    for (uint32_t y = 0; y < hp; y++) {
        const uint16_t *r0 = bayer + (2 * y) * W;
        const uint16_t *r1 = bayer + (2 * y + 1) * W;
        for (uint32_t x = 0; x < wp; x++) {
            R [y * wp + x] = (float)r0[2*x    ] * scale;
            G1[y * wp + x] = (float)r0[2*x + 1] * scale;
            G2[y * wp + x] = (float)r1[2*x    ] * scale;
            B [y * wp + x] = (float)r1[2*x + 1] * scale;
        }
    }
}

static void bayer_from_planes(const float *planes /* (4, H, W) */,
                              uint32_t H, uint32_t W, float invScale,
                              uint16_t *bayer /* (2H, 2W) — wrong, we interleave at native */)
{
    // Output bayer is (2H, 2W); the planes are already at output-half (i.e. H,W).
    // Wait — the SR model output is (1,4,2*Hp,2*Wp) where Hp = H/2. We pass H,W
    // already at the 2*Hp resolution, and the bayer is (2H, 2W). That's wrong.
    // Caller passes us per-plane H,W which is the output plane size. Bayer is
    // 2x that.
    for (uint32_t y = 0; y < H; y++) {
        uint16_t *r0 = bayer + (2 * y) * (2 * W);
        uint16_t *r1 = bayer + (2 * y + 1) * (2 * W);
        const float *R  = planes + 0 * H * W + y * W;
        const float *G1 = planes + 1 * H * W + y * W;
        const float *G2 = planes + 2 * H * W + y * W;
        const float *B  = planes + 3 * H * W + y * W;
        for (uint32_t x = 0; x < W; x++) {
            float r = R[x] * invScale;
            float g1 = G1[x] * invScale;
            float g2 = G2[x] * invScale;
            float b = B[x] * invScale;
            if (r < 0) r = 0; if (r > 16383) r = 16383;
            if (g1 < 0) g1 = 0; if (g1 > 16383) g1 = 16383;
            if (g2 < 0) g2 = 0; if (g2 > 16383) g2 = 16383;
            if (b < 0) b = 0; if (b > 16383) b = 16383;
            r0[2*x]     = (uint16_t)r;
            r0[2*x + 1] = (uint16_t)g1;
            r1[2*x]     = (uint16_t)g2;
            r1[2*x + 1] = (uint16_t)b;
        }
    }
}

// Bicubic catmull-rom upsample for one plane (H, W) → (2H, 2W).
// Single-threaded scalar; we'll optimize later.
static inline float cubic(float a, float b, float c, float d, float t) {
    // Catmull-Rom cubic interpolation
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * (
        (2.0f * b) +
        (-a + c) * t +
        (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
        (-a + 3.0f * b - 3.0f * c + d) * t3
    );
}

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline int clamp_i(int v, int hi) { return v < 0 ? 0 : (v >= hi ? hi - 1 : v); }

static void bicubic_2x_plane(const float *in, int W, int H, float *out) {
    // Output dims (2W, 2H). Standard bicubic that matches torch
    // F.interpolate(mode='bicubic', align_corners=False).
    int OW = 2 * W, OH = 2 * H;
    for (int oy = 0; oy < OH; oy++) {
        float src_y = ((float)oy + 0.5f) * 0.5f - 0.5f;
        int iy = (int)floorf(src_y);
        float fy = src_y - (float)iy;
        for (int ox = 0; ox < OW; ox++) {
            float src_x = ((float)ox + 0.5f) * 0.5f - 0.5f;
            int ix = (int)floorf(src_x);
            float fx = src_x - (float)ix;
            float col[4];
            for (int dy = -1; dy <= 2; dy++) {
                int y = clamp_i(iy + dy, H);
                float a = in[y * W + clamp_i(ix - 1, W)];
                float b = in[y * W + clamp_i(ix    , W)];
                float c = in[y * W + clamp_i(ix + 1, W)];
                float d = in[y * W + clamp_i(ix + 2, W)];
                col[dy + 1] = cubic(a, b, c, d, fx);
            }
            out[oy * OW + ox] = cubic(col[0], col[1], col[2], col[3], fy);
        }
    }
}

- (int)runOnBayer:(const uint16_t *)inBayer
            width:(uint32_t)inW height:(uint32_t)inH
         outBayer:(uint16_t *)outBayer
         outWidth:(uint32_t)outW
        outHeight:(uint32_t)outH
       blackLevel:(uint32_t)blackLevel
       whiteLevel:(uint32_t)whiteLevel
{
    // The model's input shape is (1, 4, Hp, Wp). We need inW/2 == Wp and
    // inH/2 == Hp. The Python pipeline pads to multiples of 8; we'll do the
    // same. Then output is (1, 4, 2*Hp, 2*Wp) — we crop back to (inH, inW).
    if (_inputShape.count != 4) {
        fprintf(stderr, "SuperResCNN: input shape rank=%lu unexpected\n",
                (unsigned long)_inputShape.count);
        return -1;
    }
    int Hp_expected = [_inputShape[2] intValue];
    int Wp_expected = [_inputShape[3] intValue];
    int Hp = (int)inH / 2;
    int Wp = (int)inW / 2;
    int pad_h = (Hp_expected - Hp);
    int pad_w = (Wp_expected - Wp);
    if (pad_h < 0 || pad_w < 0) {
        fprintf(stderr, "SuperResCNN: input %dx%d (planes %dx%d) exceeds model %dx%d\n",
                inW, inH, Wp, Hp, Wp_expected, Hp_expected);
        return -1;
    }
    int Hpp = Hp_expected, Wpp = Wp_expected;
    int out_Hpp = 2 * Hpp, out_Wpp = 2 * Wpp;
    int outH_target = (int)outH, outW_target = (int)outW;

    NSError *err = nil;
    // Allocate input MLMultiArray fp16.
    MLMultiArray *in = [[MLMultiArray alloc] initWithShape:_inputShape
                                                  dataType:MLMultiArrayDataTypeFloat16
                                                     error:&err];
    if (!in) { fprintf(stderr, "SuperResCNN: MLMultiArray fail: %s\n", [err.localizedDescription UTF8String]); return -1; }

    // Fill with planes (zero-padded). Use the actual stride from MLMultiArray.
    size_t plane_stride = (size_t)Hpp * Wpp;
    size_t total = 4 * plane_stride;
    // Build planes float32 first
    float *planes_f = calloc(total, sizeof(float));
    float scale = 1.0f / 16383.0f;  // matches Python pipeline normalization
    // The Python pipeline normalizes by 16383 not whiteLevel. Keep that for
    // compatibility with the trained CNN.

    {
        // Fill the live (Wp x Hp) portion of each plane; the padded region stays 0.
        for (int y = 0; y < Hp; y++) {
            for (int x = 0; x < Wp; x++) {
                int b = y * 2; int b2 = y * 2 + 1;
                uint16_t R  = inBayer[b  * inW + 2 * x];
                uint16_t G1 = inBayer[b  * inW + 2 * x + 1];
                uint16_t G2 = inBayer[b2 * inW + 2 * x];
                uint16_t Bv = inBayer[b2 * inW + 2 * x + 1];
                planes_f[0 * plane_stride + y * Wpp + x] = (float)R  * scale;
                planes_f[1 * plane_stride + y * Wpp + x] = (float)G1 * scale;
                planes_f[2 * plane_stride + y * Wpp + x] = (float)G2 * scale;
                planes_f[3 * plane_stride + y * Wpp + x] = (float)Bv * scale;
            }
        }
    }

    // Convert fp32 → fp16 via vImage (Accelerate). Single linear pass; much
    // faster than the scalar bit-twiddle loop.
    {
        vImage_Buffer src = {
            .data = planes_f, .height = 1, .width = total, .rowBytes = total * sizeof(float)
        };
        vImage_Buffer dst = {
            .data = in.dataPointer, .height = 1, .width = total, .rowBytes = total * sizeof(uint16_t)
        };
        vImageConvert_PlanarFtoPlanar16F(&src, &dst, 0);
    }

    // Predict
    MLDictionaryFeatureProvider *inProv =
        [[MLDictionaryFeatureProvider alloc]
         initWithDictionary:@{_inputName: [MLFeatureValue featureValueWithMultiArray:in]}
                      error:&err];
    if (!inProv) {
        fprintf(stderr, "SuperResCNN: feature provider fail\n");
        free(planes_f);
        return -1;
    }
    id<MLFeatureProvider> outProv = [_model predictionFromFeatures:inProv error:&err];
    if (!outProv) {
        fprintf(stderr, "SuperResCNN: predict fail: %s\n",
                [err.localizedDescription UTF8String]);
        free(planes_f);
        return -1;
    }
    MLMultiArray *residual = [outProv featureValueForName:_outputName].multiArrayValue;
    if (!residual) {
        fprintf(stderr, "SuperResCNN: no residual output\n");
        free(planes_f);
        return -1;
    }
    // residual is (1, 4, out_Hpp, out_Wpp) fp16.
    size_t out_plane = (size_t)out_Hpp * out_Wpp;

    // Bicubic baseline at 2x of padded input. Parallelize the 4 planes.
    float *baseline_f = malloc(4 * out_plane * sizeof(float));
    dispatch_apply(4, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t p) {
        bicubic_2x_plane(planes_f + p * plane_stride, Wpp, Hpp,
                         baseline_f + p * out_plane);
    });

    // Combine: y = baseline + residual_scale * residual (clamp).
    // residual_scale = 0.01 per Python pipeline.
    const float RS = 0.01f;
    // Convert residual fp16 → fp32 in one shot via vImage.
    size_t res_total = (size_t)4 * out_plane;
    float *residual_f = malloc(res_total * sizeof(float));
    {
        vImage_Buffer src = {
            .data = residual.dataPointer, .height = 1, .width = res_total,
            .rowBytes = res_total * sizeof(uint16_t)
        };
        vImage_Buffer dst = {
            .data = residual_f, .height = 1, .width = res_total,
            .rowBytes = res_total * sizeof(float)
        };
        vImageConvert_Planar16FtoPlanarF(&src, &dst, 0);
    }
    // SIMD combine using vDSP: y[i] = clamp(baseline[i] + RS * residual[i], 0, 1)
    {
        // baseline_f and residual_f are both size res_total
        // vDSP: out = a*v1 + v2 (vsma reversed args)
        vDSP_vsma(residual_f, 1, &RS, baseline_f, 1, baseline_f, 1, res_total);
        float lo = 0.0f, hi = 1.0f;
        vDSP_vclip(baseline_f, 1, &lo, &hi, baseline_f, 1, res_total);
    }
    float *combined = baseline_f;
    free(residual_f);

    // Re-bayerize the (2*Hpp) x (2*Wpp) planes back into output Bayer,
    // cropping to the requested outW x outH (which equals 2*inW x 2*inH
    // typically, but generally 2*Hp x 2*Wp).
    // outBayer is outW x outH uint16.
    float invScale = 16383.0f;
    int outH_planes = outH_target / 2;
    int outW_planes = outW_target / 2;
    for (int y = 0; y < outH_planes; y++) {
        uint16_t *row0 = outBayer + (2 * y    ) * outW_target;
        uint16_t *row1 = outBayer + (2 * y + 1) * outW_target;
        for (int x = 0; x < outW_planes; x++) {
            float r  = combined[0 * out_plane + y * out_Wpp + x] * invScale;
            float g1 = combined[1 * out_plane + y * out_Wpp + x] * invScale;
            float g2 = combined[2 * out_plane + y * out_Wpp + x] * invScale;
            float bv = combined[3 * out_plane + y * out_Wpp + x] * invScale;
            if (r  < 0) r  = 0; if (r  > 16383) r  = 16383;
            if (g1 < 0) g1 = 0; if (g1 > 16383) g1 = 16383;
            if (g2 < 0) g2 = 0; if (g2 > 16383) g2 = 16383;
            if (bv < 0) bv = 0; if (bv > 16383) bv = 16383;
            row0[2 * x    ] = (uint16_t)r;
            row0[2 * x + 1] = (uint16_t)g1;
            row1[2 * x    ] = (uint16_t)g2;
            row1[2 * x + 1] = (uint16_t)bv;
        }
    }
    free(planes_f);
    free(baseline_f);
    return 0;
}

@end
