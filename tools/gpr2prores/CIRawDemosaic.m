// CIRawDemosaic.m — see header.
//
// Per-frame DNG synthesis used to dominate runtime: every frame built a fresh
// ~91 MB DNG NSData via CIDNGBuilder and handed it to
// [CIRAWFilter filterWithImageData:], which made Apple's DNG parser walk a
// fresh TIFF/IFD on every frame. On 50 MP Z8 frames that path measured
// ~55 ms/frame steady-state.
//
// New path uses [CIRAWFilter filterWithCVPixelBuffer:properties:]:
//
//   * Build one CVPixelBufferPool once at init, format
//     kCVPixelFormatType_14Bayer_<CFA> (IOSurface-backed, Metal-compatible).
//   * Build the metadata properties dict once at init by synthesizing one
//     minimal DNG (via CIDNGBuilder, all camera-profile tags inherited from
//     the template DNG) at the exact buffer dims, then parsing it through
//     ImageIO to obtain a CGImageSource-style properties dict. CIRAWFilter
//     accepts exactly this dict shape and uses it to pick a DNG decoder.
//   * Per frame: acquire a pooled buffer, memcpy bayer into it, hand to
//     CIRAWFilter, render. No DNG construction. No DNG parse.
//
// Measured drop on 50 MP Z8 codec-decimated bayer at UHD output:
//   ~55 ms → ~25 ms (~2.2×). Visual output matches the old path's
//   colorimetry; the new path tends to read slightly brighter because the
//   filter's exposure heuristics see the buffer at its actual bp16-class
//   format instead of a TIFF strip.
//
// Why a *synthesized* probe DNG for the properties dict instead of just
// reading the template file's properties directly?
//   1. NEF / .nef inputs return no kCGImagePropertyDNGDictionary at all
//      from ImageIO. CIRAWFilter then refuses to pick a decoder.
//   2. The template DNG's dims may not match our buffer's dims (we may be
//      consuming codec-decimated bayer at 2× smaller dims than the source).
//      CIRAWFilter cross-checks dims; a mismatch yields outputImage=nil and
//      supportedDecoderVersions={None}.
// Synthesizing a minimal DNG at our buffer's exact dims (while inheriting
// the template's color-science tags via CIDNGBuilder) gives a properties
// dict that is dimensionally consistent and contains a real DNG dict.

#import "CIRawDemosaic.h"
#import "CIDNGBuilder.h"

#import <CoreImage/CoreImage.h>
#import <CoreImage/CIRAWFilter.h>
#import <ImageIO/ImageIO.h>
#import <math.h>

@implementation CIRawDemosaic {
    id<MTLDevice>     _device;
    CIContext        *_ciContext;
    uint32_t          _outW, _outH;
    uint32_t          _sensorW, _sensorH;
    CGColorSpaceRef   _outCS;

    // Pool of 14Bayer<CFA> pixel buffers, sized to sensorW × sensorH.
    CVPixelBufferPoolRef _bayerPool;
    // Constant metadata dict passed to filterWithCVPixelBuffer:properties:
    // every frame. Same shape CGImageSourceCopyProperties returns for a DNG.
    NSDictionary     *_filterProperties;
    BOOL              _missionLook;
    CGFloat           _missionCropScale;
    CGFloat           _missionExposure;
    CGFloat           _missionBaselineExposure;
    CGFloat           _missionBoost;
    CGFloat           _missionBoostShadow;
    CGFloat           _missionShadowBias;
    CGFloat           _missionLocalTone;
    BOOL              _missionGuardedTone;
    BOOL              _missionLocalToneCpu;
    CGFloat           _missionToneMaxRatioScale;
    CGFloat           _missionToneShadowScale;
    uint32_t          _missionLocalDownsample;
}

typedef struct {
    float shadowAdapt;
    float targetMed;
    float pivot;
    float highlight;
    float hpivot;
    float minRatio;
    float maxRatio;
    float sat;
    float liftDesat;
    float localTarget;
    float localAmount;
    float localMax;
    float localPivot;
    float localMaskPower;
    float localHighlightGuard;
    float localSat;
    float localDesat;
} MissionToneParams;

// Pick the kCVPixelFormatType_14Bayer_* CFA constant matching our enum.
// DNGInfo->cfaPattern: 0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR.
static OSType bayerFormatForCFA(uint32_t cfa) {
    switch (cfa) {
        case 0: return kCVPixelFormatType_14Bayer_RGGB;
        case 1: return kCVPixelFormatType_14Bayer_GBRG;
        case 2: return kCVPixelFormatType_14Bayer_GRBG;
        case 3: return kCVPixelFormatType_14Bayer_BGGR;
        default: return kCVPixelFormatType_14Bayer_RGGB;
    }
}

// Build the per-session properties dict. See file header for the rationale
// behind synthesizing a probe DNG at the buffer's exact dims.
static NSDictionary *buildFilterProperties(const DNGInfo *info,
                                            uint32_t sensorW,
                                            uint32_t sensorH,
                                            NSString *templateDngPath)
{
    NSDictionary *result = nil;
    CIDNGBuilder *probeBuilder = [[CIDNGBuilder alloc] initWithInfo:info
                                                              width:sensorW
                                                             height:sensorH
                                                    templateDngPath:templateDngPath];
    if (!probeBuilder) return nil;

    size_t bayerBytes = (size_t)sensorW * sensorH * 2;
    void *probeBayer = calloc(1, bayerBytes);
    NSData *probeDng = probeBayer ? [probeBuilder dngFromBayerBytes:probeBayer length:bayerBytes] : nil;
    free(probeBayer);
    if (!probeDng) return nil;

    CGImageSourceRef src = CGImageSourceCreateWithData((__bridge CFDataRef)probeDng, NULL);
    if (!src) return nil;
    NSDictionary *file = (__bridge_transfer NSDictionary *)CGImageSourceCopyProperties(src, NULL);
    NSDictionary *idx0 = (__bridge_transfer NSDictionary *)CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
    CFRelease(src);

    NSMutableDictionary *merged = [NSMutableDictionary dictionary];
    if (file) [merged addEntriesFromDictionary:file];
    if (idx0) {
        for (NSString *k in idx0) {
            if (!merged[k]) merged[k] = idx0[k];
        }
    }
    if (merged.count) result = merged;
    return result;
}

static CGFloat envCGFloat(const char *name, CGFloat fallback)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return fallback;
    char *end = NULL;
    double parsed = strtod(v, &end);
    if (end == v) return fallback;
    return (CGFloat)parsed;
}

static BOOL envBool(const char *name, BOOL fallback)
{
    const char *v = getenv(name);
    if (!v || !v[0]) return fallback;
    return atoi(v) != 0;
}

static float clampf_mission(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t histPercentile(const uint32_t hist[256], uint64_t total, float pct)
{
    if (total == 0) return 0;
    uint64_t target = (uint64_t)((pct / 100.0f) * (float)(total - 1));
    uint64_t acc = 0;
    for (uint32_t i = 0; i < 256; i++) {
        acc += hist[i];
        if (acc > target) return i;
    }
    return 255;
}

static MissionToneParams chooseMissionTone(float median, float p90)
{
    const float root = 0.1694682389497757f;
    const float darkP90 = 0.22304511070251465f;
    const float mid = 0.2946549206972122f;
    if (median <= root) {
        if (p90 <= darkP90) {
            return (MissionToneParams){
                .shadowAdapt = 1.9f, .targetMed = 0.54f, .pivot = 0.70f,
                .highlight = 0.04f, .hpivot = 0.62f, .minRatio = 0.65f,
                .maxRatio = 3.8f, .sat = 0.98f, .liftDesat = 0.25f,
                .localTarget = 0.78f, .localAmount = 1.05f, .localMax = 1.7f,
                .localPivot = 0.7f, .localMaskPower = 1.4f,
                .localHighlightGuard = 1.0f, .localSat = 1.04f, .localDesat = 0.4f,
            };
        }
        return (MissionToneParams){
            .shadowAdapt = 1.9f, .targetMed = 0.50f, .pivot = 0.86f,
            .highlight = 0.08f, .hpivot = 0.80f, .minRatio = 0.65f,
            .maxRatio = 2.4f, .sat = 0.98f, .liftDesat = 0.10f,
            .localTarget = 0.78f, .localAmount = 1.05f, .localMax = 1.7f,
            .localPivot = 0.7f, .localMaskPower = 1.4f,
            .localHighlightGuard = 1.0f, .localSat = 0.98f, .localDesat = 0.05f,
        };
    }
    if (median <= mid) {
        return (MissionToneParams){
            .shadowAdapt = 2.5f, .targetMed = 0.58f, .pivot = 0.94f,
            .highlight = 0.0f, .hpivot = 0.88f, .minRatio = 0.65f,
            .maxRatio = 1.7f, .sat = 0.90f, .liftDesat = 0.10f,
            .localTarget = 0.62f, .localAmount = 0.45f, .localMax = 1.1f,
            .localPivot = 0.7f, .localMaskPower = 1.4f,
            .localHighlightGuard = 0.78f, .localSat = 1.04f, .localDesat = 0.4f,
        };
    }
    return (MissionToneParams){
        .shadowAdapt = 2.5f, .targetMed = 0.42f, .pivot = 0.70f,
        .highlight = 0.25f, .hpivot = 0.62f, .minRatio = 0.65f,
        .maxRatio = 1.4f, .sat = 1.10f, .liftDesat = 0.10f,
        .localTarget = 0.55f, .localAmount = 0.45f, .localMax = 1.7f,
        .localPivot = 0.5f, .localMaskPower = 1.4f,
        .localHighlightGuard = 0.78f, .localSat = 1.04f, .localDesat = 0.65f,
    };
}

static void boxBlurFloat(const float *src, float *dst, int width, int height, int radius)
{
    if (radius <= 0) {
        memcpy(dst, src, (size_t)width * (size_t)height * sizeof(float));
        return;
    }
    int window = radius * 2 + 1;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float acc = 0.0f;
            for (int ky = -radius; ky <= radius; ky++) {
                int sy = y + ky;
                if (sy < 0) sy = 0;
                if (sy >= height) sy = height - 1;
                const float *row = src + (size_t)sy * (size_t)width;
                for (int kx = -radius; kx <= radius; kx++) {
                    int sx = x + kx;
                    if (sx < 0) sx = 0;
                    if (sx >= width) sx = width - 1;
                    acc += row[sx];
                }
            }
            dst[(size_t)y * (size_t)width + (size_t)x] = acc / (float)(window * window);
        }
    }
}

static void applyMissionLocalTone(CVPixelBufferRef pb,
                                  MissionToneParams t,
                                  uint32_t downsample)
{
    uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    size_t width = CVPixelBufferGetWidth(pb);
    size_t height = CVPixelBufferGetHeight(pb);
    if (!base || width == 0 || height == 0) return;

    uint32_t ds = downsample < 1 ? 1 : downsample;
    if (ds > 16) ds = 16;
    int mapW = (int)((width + ds - 1) / ds);
    int mapH = (int)((height + ds - 1) / ds);
    size_t mapCount = (size_t)mapW * (size_t)mapH;
    float *low = (float *)calloc(mapCount, sizeof(float));
    float *tmp = (float *)calloc(mapCount, sizeof(float));
    if (!low || !tmp) {
        free(low);
        free(tmp);
        return;
    }

    for (int my = 0; my < mapH; my++) {
        size_t y0 = (size_t)my * (size_t)ds;
        size_t y1 = y0 + ds;
        if (y1 > height) y1 = height;
        for (int mx = 0; mx < mapW; mx++) {
            size_t x0 = (size_t)mx * (size_t)ds;
            size_t x1 = x0 + ds;
            if (x1 > width) x1 = width;
            float acc = 0.0f;
            size_t count = 0;
            for (size_t y = y0; y < y1; y++) {
                uint8_t *row = base + y * stride;
                for (size_t x = x0; x < x1; x++) {
                    uint8_t *p = row + x * 4; // BGRA
                    acc += (0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0]) / 255.0f;
                    count++;
                }
            }
            low[(size_t)my * (size_t)mapW + (size_t)mx] = count ? acc / (float)count : 0.0f;
        }
    }

    int radius = (int)lroundf(8.0f / (float)ds);
    if (radius < 1) radius = 1;
    boxBlurFloat(low, tmp, mapW, mapH, radius);
    boxBlurFloat(tmp, low, mapW, mapH, radius);
    boxBlurFloat(low, tmp, mapW, mapH, radius);

    for (size_t y = 0; y < height; y++) {
        uint8_t *row = base + y * stride;
        int my = (int)(y / ds);
        if (my >= mapH) my = mapH - 1;
        for (size_t x = 0; x < width; x++) {
            int mx = (int)(x / ds);
            if (mx >= mapW) mx = mapW - 1;
            float lowBlurred = tmp[(size_t)my * (size_t)mapW + (size_t)mx];
            uint8_t *p = row + x * 4; // BGRA
            float b = (float)p[0] / 255.0f;
            float g = (float)p[1] / 255.0f;
            float r = (float)p[2] / 255.0f;
            float lowSource = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float desired = clampf_mission(t.localTarget / fmaxf(lowBlurred, 1e-4f),
                                           1.0f,
                                           t.localMax);
            float localMask = clampf_mission((t.localPivot - lowBlurred) / fmaxf(t.localPivot, 1e-4f),
                                             0.0f,
                                             1.0f);
            localMask = powf(localMask, t.localMaskPower);
            float highlightGuard = clampf_mission((t.localHighlightGuard - lowSource) /
                                                  fmaxf(t.localHighlightGuard, 1e-4f),
                                                  0.0f,
                                                  1.0f);
            float localAmount = t.localAmount * localMask * highlightGuard;
            float localRatio = 1.0f + localAmount * (desired - 1.0f);
            r = clampf_mission(r * localRatio, 0.0f, 1.0f);
            g = clampf_mission(g * localRatio, 0.0f, 1.0f);
            b = clampf_mission(b * localRatio, 0.0f, 1.0f);
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float satEff = t.localSat * (1.0f - t.localDesat * localAmount);
            r = clampf_mission(lum + (r - lum) * satEff, 0.0f, 1.0f);
            g = clampf_mission(lum + (g - lum) * satEff, 0.0f, 1.0f);
            b = clampf_mission(lum + (b - lum) * satEff, 0.0f, 1.0f);
            p[0] = (uint8_t)clampf_mission(b * 255.0f + 0.5f, 0.0f, 255.0f);
            p[1] = (uint8_t)clampf_mission(g * 255.0f + 0.5f, 0.0f, 255.0f);
            p[2] = (uint8_t)clampf_mission(r * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }

    free(low);
    free(tmp);
}

static void applyMissionGuardedTone(CVPixelBufferRef pb,
                                    float maxRatioScale,
                                    float shadowScale,
                                    BOOL localToneCpu,
                                    uint32_t localDownsample)
{
    CVPixelBufferLockBaseAddress(pb, 0);
    uint8_t *base = (uint8_t *)CVPixelBufferGetBaseAddress(pb);
    size_t stride = CVPixelBufferGetBytesPerRow(pb);
    size_t width = CVPixelBufferGetWidth(pb);
    size_t height = CVPixelBufferGetHeight(pb);
    if (!base || width == 0 || height == 0) {
        CVPixelBufferUnlockBaseAddress(pb, 0);
        return;
    }

    uint32_t hist[256] = {0};
    for (size_t y = 0; y < height; y++) {
        uint8_t *row = base + y * stride;
        for (size_t x = 0; x < width; x++) {
            uint8_t *p = row + x * 4; // BGRA
            float yy = 0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0];
            hist[(uint8_t)clampf_mission(yy + 0.5f, 0.0f, 255.0f)]++;
        }
    }
    uint64_t total = (uint64_t)width * (uint64_t)height;
    float lo = (float)histPercentile(hist, total, 0.5f) / 255.0f;
    float hi = (float)histPercentile(hist, total, 99.8f) / 255.0f;
    float median = (float)histPercentile(hist, total, 50.0f) / 255.0f;
    float p90 = (float)histPercentile(hist, total, 90.0f) / 255.0f;
    MissionToneParams t = chooseMissionTone(median, p90);
    t.maxRatio *= fmaxf(maxRatioScale, 0.1f);
    float span = fmaxf(hi - lo, 1e-4f);
    float shadow = t.shadowAdapt * shadowScale * fmaxf(0.0f, t.targetMed - median);

    for (size_t y = 0; y < height; y++) {
        uint8_t *row = base + y * stride;
        for (size_t x = 0; x < width; x++) {
            uint8_t *p = row + x * 4; // BGRA
            float b = (float)p[0] / 255.0f;
            float g = (float)p[1] / 255.0f;
            float r = (float)p[2] / 255.0f;
            float lum0 = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float yy = clampf_mission((lum0 - lo) / span, 0.0f, 1.0f);
            if (shadow > 0.0f) {
                float mask = clampf_mission((t.pivot - yy) / fmaxf(t.pivot, 1e-4f), 0.0f, 1.0f);
                yy = yy + shadow * mask * (1.0f - yy);
            }
            if (t.highlight > 0.0f) {
                float mask = clampf_mission((yy - t.hpivot) / fmaxf(1.0f - t.hpivot, 1e-4f), 0.0f, 1.0f);
                yy = yy - t.highlight * mask * yy * (1.0f - yy);
            }
            yy = clampf_mission(yy, 0.0f, 1.0f);
            float ratio = yy / fmaxf(lum0, 1e-4f);
            ratio = clampf_mission(ratio, t.minRatio, t.maxRatio);
            r = clampf_mission(r * ratio, 0.0f, 1.0f);
            g = clampf_mission(g * ratio, 0.0f, 1.0f);
            b = clampf_mission(b * ratio, 0.0f, 1.0f);
            float lum1 = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            float lift = clampf_mission((ratio - 1.0f) / fmaxf(t.maxRatio - 1.0f, 1e-4f), 0.0f, 1.0f);
            float satEff = t.sat * (1.0f - t.liftDesat * lift);
            r = clampf_mission(lum1 + (r - lum1) * satEff, 0.0f, 1.0f);
            g = clampf_mission(lum1 + (g - lum1) * satEff, 0.0f, 1.0f);
            b = clampf_mission(lum1 + (b - lum1) * satEff, 0.0f, 1.0f);
            p[0] = (uint8_t)clampf_mission(b * 255.0f + 0.5f, 0.0f, 255.0f);
            p[1] = (uint8_t)clampf_mission(g * 255.0f + 0.5f, 0.0f, 255.0f);
            p[2] = (uint8_t)clampf_mission(r * 255.0f + 0.5f, 0.0f, 255.0f);
        }
    }
    if (localToneCpu) {
        applyMissionLocalTone(pb, t, localDownsample);
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
}

- (nullable instancetype)initWithDevice:(id<MTLDevice>)device
                            sensorWidth:(uint32_t)sensorW
                           sensorHeight:(uint32_t)sensorH
                              outWidth:(uint32_t)outW
                             outHeight:(uint32_t)outH
                                   info:(const DNGInfo *)info
                         templateDngPath:(nullable NSString *)templateDngPath
                                lookMode:(nullable NSString *)lookMode
{
    self = [super init];
    if (!self) return nil;
    _device = device;
    _sensorW = sensorW;
    _sensorH = sensorH;
    _outW = outW;
    _outH = outH;
    _missionLook = [[lookMode lowercaseString] isEqualToString:@"mission1"];
    _missionCropScale = envCGFloat("GPR_MISSION_LOOK_CROP_SCALE", 1.035);
    _missionExposure = envCGFloat("GPR_MISSION_LOOK_EXPOSURE", 0.0);
    _missionBaselineExposure = envCGFloat("GPR_MISSION_LOOK_BASELINE_EXPOSURE", 0.0);
    _missionBoost = envCGFloat("GPR_MISSION_LOOK_BOOST", 1.0);
    _missionBoostShadow = envCGFloat("GPR_MISSION_LOOK_BOOST_SHADOW", 0.0);
    _missionShadowBias = envCGFloat("GPR_MISSION_LOOK_SHADOW_BIAS", 0.0);
    _missionLocalTone = envCGFloat("GPR_MISSION_LOOK_LOCAL_TONE", 0.0);
    _missionGuardedTone = envBool("GPR_MISSION_LOOK_GUARDED_TONE", YES);
    _missionLocalToneCpu = envBool("GPR_MISSION_LOOK_LOCAL_CPU", NO);
    _missionToneMaxRatioScale = envCGFloat("GPR_MISSION_LOOK_TONE_MAX_RATIO_SCALE", 1.5);
    _missionToneShadowScale = envCGFloat("GPR_MISSION_LOOK_TONE_SHADOW_SCALE", 0.8);
    _missionLocalDownsample = (uint32_t)envCGFloat("GPR_MISSION_LOOK_LOCAL_DOWNSAMPLE", 4.0);
    if (_missionLocalDownsample < 1) _missionLocalDownsample = 1;

    // Verify the API path exists on this OS. CIRAWFilter
    // +filterWithCVPixelBuffer:properties: is macOS 12 / iOS 15.
    if (![CIRAWFilter respondsToSelector:@selector(filterWithCVPixelBuffer:properties:)]) {
        fprintf(stderr, "CIRawDemosaic: filterWithCVPixelBuffer:properties: unavailable on this OS\n");
        return nil;
    }

    // Metal-backed CIContext shares textures with our pipeline.
    _ciContext = [CIContext contextWithMTLDevice:device options:@{
        kCIContextCacheIntermediates: @NO,
    }];
    if (!_ciContext) {
        fprintf(stderr, "CIRawDemosaic: failed to create Metal CIContext\n");
        return nil;
    }

    _outCS = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    _filterProperties = buildFilterProperties(info, sensorW, sensorH, templateDngPath);
    if (!_filterProperties) {
        fprintf(stderr, "CIRawDemosaic: failed to build filter properties from template %s\n",
                templateDngPath ? [templateDngPath UTF8String] : "(nil)");
        return nil;
    }
    // Sanity check: the dict should include a TIFF dict and a DNG dict.
    if (!_filterProperties[(NSString *)kCGImagePropertyDNGDictionary]) {
        fprintf(stderr, "CIRawDemosaic: WARNING - properties dict has no DNG entry; "
                "CIRAWFilter is likely to reject the buffer\n");
    }

    // Build a CVPixelBufferPool of 14Bayer<CFA> sized to our buffer dims.
    // The 14Bayer formats are the CFA-bearing format that CIRAWFilter's
    // CV-pixel-buffer ingest path consumes; the format-fourcc carries the
    // CFA pattern and CIRAWFilter combines that with the properties dict
    // to select its DNG decoder.
    OSType bayerFormat = bayerFormatForCFA(info->cfaPattern);
    NSDictionary *pixelAttrs = @{
        (NSString *)kCVPixelBufferPixelFormatTypeKey: @(bayerFormat),
        (NSString *)kCVPixelBufferWidthKey: @(sensorW),
        (NSString *)kCVPixelBufferHeightKey: @(sensorH),
        (NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        // Hint a tight 2-byte alignment so the per-row stride matches our
        // natural width*2 bytes. With the default 64-byte alignment we get
        // dstStride=8320 vs srcStride=8280 (4140-pixel rows), forcing a
        // per-row memcpy that costs ~1.3 ms / frame for the 1× bayer (4×
        // more for 8K). Apple may still pad — verify with the runtime log.
        (NSString *)kCVPixelBufferBytesPerRowAlignmentKey: @(2),
    };
    NSDictionary *poolAttrs = @{
        (NSString *)kCVPixelBufferPoolMinimumBufferCountKey: @(2),
    };
    CVReturn r = CVPixelBufferPoolCreate(NULL,
                                          (__bridge CFDictionaryRef)poolAttrs,
                                          (__bridge CFDictionaryRef)pixelAttrs,
                                          &_bayerPool);
    if (r != kCVReturnSuccess || !_bayerPool) {
        fprintf(stderr, "CIRawDemosaic: CVPixelBufferPoolCreate failed (r=%d)\n", r);
        return nil;
    }

    fprintf(stderr,
            "CIRawDemosaic: ready (%ux%u → %ux%u via filterWithCVPixelBuffer, "
            "fmt='%c%c%c%c' cfa=%u black=%u white=%u look=%s)\n",
            sensorW, sensorH, outW, outH,
            (char)((bayerFormat >> 24) & 0xff), (char)((bayerFormat >> 16) & 0xff),
            (char)((bayerFormat >> 8) & 0xff),  (char)(bayerFormat & 0xff),
            info->cfaPattern, info->blackLevel, info->whiteLevel,
            _missionLook ? "mission1" : "none");
    if (_missionLook) {
        fprintf(stderr,
                "CIRawDemosaic: mission look crop=%.4f exposure=%.3f baseline=%.3f "
                "boost=%.3f boostShadow=%.3f shadowBias=%.3f localTone=%.3f\n",
                (double)_missionCropScale, (double)_missionExposure,
                (double)_missionBaselineExposure, (double)_missionBoost,
                (double)_missionBoostShadow, (double)_missionShadowBias,
                (double)_missionLocalTone);
        fprintf(stderr, "CIRawDemosaic: mission guardedTone=%s\n",
                _missionGuardedTone ? "on" : "off");
        fprintf(stderr, "CIRawDemosaic: mission tone maxRatioScale=%.3f shadowScale=%.3f\n",
                (double)_missionToneMaxRatioScale, (double)_missionToneShadowScale);
    }

    return self;
}

- (void)dealloc {
    if (_outCS) CGColorSpaceRelease(_outCS);
    if (_bayerPool) {
        CVPixelBufferPoolRelease(_bayerPool);
        _bayerPool = NULL;
    }
}

// Common back half: take a filled bayer CVPixelBuffer and render through
// CIRAWFilter into the caller's destination CVPixelBuffer. Used by both the
// legacy `encode:bayer:...` path (which fills bayerPB via memcpy) and the
// new `encodeFromBayerPixelBuffer:` zero-copy path.
- (void)_renderFromBayerPB:(CVPixelBufferRef)bayerPB outPixelBuffer:(CVPixelBufferRef)pb {
    // Hand to CIRAWFilter. No DNG construction, no DNG parse.
    CIRAWFilter *raw = [CIRAWFilter filterWithCVPixelBuffer:bayerPB
                                                 properties:_filterProperties];
    if (!raw) {
        fprintf(stderr, "CIRawDemosaic: filterWithCVPixelBuffer returned nil\n");
        return;
    }

    // Fast-path settings: draft mode, no denoise/sharpen/etc.
    CGFloat scale = (CGFloat)_outW / (CGFloat)_sensorW;
    if (scale <= 0 || scale > 1.0) scale = 1.0;  // CIRAWFilter clamps to (0,1]
    raw.scaleFactor = (float)scale;
    raw.draftModeEnabled = YES;
    raw.luminanceNoiseReductionAmount = 0.0f;
    raw.colorNoiseReductionAmount = 0.0f;
    raw.sharpnessAmount = 0.0f;
    raw.detailAmount = 0.0f;
    raw.moireReductionAmount = 0.0f;
    raw.localToneMapAmount = 0.0f;
    if (_missionLook) {
        raw.exposure = (float)_missionExposure;
        raw.baselineExposure = (float)_missionBaselineExposure;
        raw.boostAmount = (float)_missionBoost;
        raw.boostShadowAmount = (float)_missionBoostShadow;
        raw.shadowBias = (float)_missionShadowBias;
        raw.localToneMapAmount = (float)_missionLocalTone;
    }

    CIImage *image = raw.outputImage;
    if (!image) {
        fprintf(stderr, "CIRawDemosaic: outputImage nil\n");
        return;
    }

    // Translate to origin, then scale-to-fit + center-crop to (outW, outH).
    CGRect extent = image.extent;
    CGAffineTransform xform = CGAffineTransformMakeTranslation(-extent.origin.x, -extent.origin.y);
    CIImage *moved = [image imageByApplyingTransform:xform];

    if (_missionLook && _missionCropScale > 1.0001) {
        CGRect e = moved.extent;
        CGFloat cropW = e.size.width / _missionCropScale;
        CGFloat cropH = e.size.height / _missionCropScale;
        CGFloat cropX = e.origin.x + (e.size.width - cropW) * 0.5;
        CGFloat cropY = e.origin.y + (e.size.height - cropH) * 0.5;
        moved = [moved imageByCroppingToRect:CGRectMake(cropX, cropY, cropW, cropH)];
        moved = [moved imageByApplyingTransform:CGAffineTransformMakeTranslation(-cropX, -cropY)];
    }

    CGRect movedExtent = moved.extent;
    CGFloat sx = (CGFloat)_outW / movedExtent.size.width;
    CGFloat sy = (CGFloat)_outH / movedExtent.size.height;
    CGFloat finalScale = MAX(sx, sy);
    CIImage *scaled = moved;
    if (fabs(finalScale - 1.0) > 1e-4) {
        scaled = [moved imageByApplyingTransform:CGAffineTransformMakeScale(finalScale, finalScale)];
    }
    CGRect scaledExtent = scaled.extent;
    CGFloat cropX = (scaledExtent.size.width  - (CGFloat)_outW) * 0.5;
    CGFloat cropY = (scaledExtent.size.height - (CGFloat)_outH) * 0.5;
    CIImage *cropped = [scaled imageByApplyingTransform:CGAffineTransformMakeTranslation(-cropX, -cropY)];
    cropped = [cropped imageByCroppingToRect:CGRectMake(0, 0, _outW, _outH)];

    // Render directly into the caller's CVPixelBuffer (IOSurface-backed).
    [_ciContext render:cropped
        toCVPixelBuffer:pb
                 bounds:CGRectMake(0, 0, _outW, _outH)
             colorSpace:_outCS];
    if (_missionLook && _missionGuardedTone) {
        applyMissionGuardedTone(pb,
                                (float)_missionToneMaxRatioScale,
                                (float)_missionToneShadowScale,
                                _missionLocalToneCpu,
                                _missionLocalDownsample);
    }
}

- (void)encode:(nullable id<MTLCommandBuffer>)cb
         bayer:(const uint16_t *)bayer
         width:(uint32_t)w
        height:(uint32_t)h
outPixelBuffer:(CVPixelBufferRef)pb
{
    (void)cb;  // CIContext drives its own Metal submissions.

    if (w != _sensorW || h != _sensorH) {
        fprintf(stderr, "CIRawDemosaic: bayer dims %ux%u differ from configured %ux%u\n",
                w, h, _sensorW, _sensorH);
        return;
    }

    // 1) Pull a pooled Bayer buffer.
    CVPixelBufferRef bayerPB = NULL;
    CVReturn r = CVPixelBufferPoolCreatePixelBuffer(NULL, _bayerPool, &bayerPB);
    if (r != kCVReturnSuccess || !bayerPB) {
        fprintf(stderr, "CIRawDemosaic: pool alloc failed (r=%d)\n", r);
        return;
    }

    // 2) Copy the bayer plane in (single-plane, 16 bits/pixel).
    // The CVPixelBuffer's bytes-per-row is padded to >=64-byte alignment by
    // CoreVideo (Apple ignores the bytesPerRowAlignmentKey hint and rounds up
    // anyway). For our typical 4140-wide bayer that's 8320 vs natural 8280,
    // so we have to do a per-row memcpy. Parallelize across 4 CPU cores —
    // serial memcpy took ~1.3 ms, the 4-way parallel version takes ~0.3-0.5 ms
    // AND overlaps better with the GPU CNN finishing on the prior frame.
    CVPixelBufferLockBaseAddress(bayerPB, 0);
    uint8_t *dst = (uint8_t *)CVPixelBufferGetBaseAddress(bayerPB);
    size_t dstStride = CVPixelBufferGetBytesPerRow(bayerPB);
    size_t srcStride = (size_t)w * 2;
    if (dstStride == srcStride) {
        memcpy(dst, bayer, (size_t)h * srcStride);
    } else {
        dispatch_apply(4, DISPATCH_APPLY_AUTO, ^(size_t chunk) {
            size_t y0 = (h * chunk) / 4;
            size_t y1 = (h * (chunk + 1)) / 4;
            for (size_t y = y0; y < y1; y++) {
                memcpy(dst + y * dstStride,
                       ((const uint8_t *)bayer) + y * srcStride,
                       srcStride);
            }
        });
    }
    CVPixelBufferUnlockBaseAddress(bayerPB, 0);

    [self _renderFromBayerPB:bayerPB outPixelBuffer:pb];
    CVPixelBufferRelease(bayerPB);
}

- (void)encodeFromBayerPixelBuffer:(CVPixelBufferRef)bayerPB
                    outPixelBuffer:(CVPixelBufferRef)pb
{
    if (!bayerPB || !pb) {
        fprintf(stderr, "CIRawDemosaic: encodeFromBayerPixelBuffer: nil arg\n");
        return;
    }
    size_t w = CVPixelBufferGetWidth(bayerPB);
    size_t h = CVPixelBufferGetHeight(bayerPB);
    if (w != _sensorW || h != _sensorH) {
        fprintf(stderr, "CIRawDemosaic: bayerPB dims %zux%zu differ from configured %ux%u\n",
                w, h, _sensorW, _sensorH);
        return;
    }
    [self _renderFromBayerPB:bayerPB outPixelBuffer:pb];
}

@end
