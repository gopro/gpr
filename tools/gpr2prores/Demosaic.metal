// Demosaic.metal — bilinear Bayer→BGRA8 on Apple GPU.
//
// One thread per output pixel. For each pixel we figure out which of
// the 2×2 RGGB positions we sit at (using gid % 2) and reconstruct the
// missing color channels by averaging neighbors. White balance + gamma
// happen here too to keep the pipeline simple.
//
// Output is BGRA8 written to a Metal texture (which is the IOSurface
// underlying the CVPixelBuffer the ProRes encoder consumes).
//
// Bayer pattern (0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR) tells us which of the 2×2
// positions corresponds to which color. We pre-compute that on CPU and pass
// it as a constant.

#include <metal_stdlib>
using namespace metal;

struct DemosaicParams {
    uint   width;
    uint   height;
    uint   cfaPattern;   // 0=RGGB, 1=GBRG, 2=GRBG, 3=BGGR
    uint   blackLevel;
    float  whiteScale;   // 1.0 / (white - black)
    float  gammaInvA;    // sRGB-ish gamma encode params
    float  gammaInvB;
    // White balance multipliers (camera neutral). 1,1,1 if not applied here.
    float  wbR;
    float  wbG;
    float  wbB;
    // Simple per-channel gain (e.g. for brightness match)
    float  gainR;
    float  gainG;
    float  gainB;
    // 3x3 camera-RGB → sRGB matrix (applied after WB).
    float  m00, m01, m02;
    float  m10, m11, m12;
    float  m20, m21, m22;
};

// Read a single Bayer sample with mirror-edge clamping. The input is a
// uint16 buffer.
static inline uint sample_u16(device const ushort *src, int x, int y, int w, int h) {
    x = clamp(x, 0, w - 1);
    y = clamp(y, 0, h - 1);
    return (uint)src[y * w + x];
}

// Convert linear [0,1] to sRGB-ish output. We use Rec.709-like gamma to
// roughly match the rawpy postprocess(gamma=(2.222, 4.5), output_color=sRGB)
// path. Not bit-exact but close.
static inline float linear_to_gamma(float v) {
    v = clamp(v, 0.0f, 1.0f);
    // Approx Rec.709 transfer (gamma 2.2, linear toe)
    if (v < 0.018f) return v * 4.5f;
    return 1.099f * pow(v, 1.0f/2.222f) - 0.099f;
}

// Which (x,y) parity is R / G / B given the cfaPattern? Returns:
//   0 = R, 1 = G (row), 2 = G (col), 3 = B
// for the 2×2 cell positions (0,0), (0,1), (1,0), (1,1).
// We bake the mapping into a small lookup so the kernel doesn't branch heavily.
constant uint kCFAMap[4][4] = {
    {0, 1, 2, 3},  // RGGB
    {1, 3, 0, 2},  // GBRG: G B / R G
    {1, 0, 3, 2},  // GRBG: G R / B G
    {3, 2, 1, 0},  // BGGR: B G / G R
};

static inline uint pos_to_color(uint cfa, uint xp, uint yp) {
    return kCFAMap[cfa][2*yp + xp];
}

kernel void demosaic_bilinear(
    device const ushort *bayer       [[buffer(0)]],
    constant DemosaicParams &P       [[buffer(1)]],
    texture2d<float, access::write> out [[texture(0)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= P.width || gid.y >= P.height) return;
    int x = (int)gid.x;
    int y = (int)gid.y;
    int W = (int)P.width;
    int H = (int)P.height;

    uint xp = gid.x & 1;
    uint yp = gid.y & 1;
    uint c  = pos_to_color(P.cfaPattern, xp, yp);

    float r, g, b;
    float center = (float)sample_u16(bayer, x, y, W, H);
    float n  = (float)sample_u16(bayer, x,   y-1, W, H);
    float s  = (float)sample_u16(bayer, x,   y+1, W, H);
    float w_ = (float)sample_u16(bayer, x-1, y,   W, H);
    float e  = (float)sample_u16(bayer, x+1, y,   W, H);
    float nw = (float)sample_u16(bayer, x-1, y-1, W, H);
    float ne = (float)sample_u16(bayer, x+1, y-1, W, H);
    float sw = (float)sample_u16(bayer, x-1, y+1, W, H);
    float se = (float)sample_u16(bayer, x+1, y+1, W, H);

    switch (c) {
        case 0: // R site
            r = center;
            g = 0.25f * (n + s + w_ + e);
            b = 0.25f * (nw + ne + sw + se);
            break;
        case 1: // G in R row (Gr): R neighbors are E/W, B neighbors are N/S
            g = center;
            r = 0.5f * (w_ + e);
            b = 0.5f * (n + s);
            break;
        case 2: // G in B row (Gb): B neighbors are E/W, R neighbors are N/S
            g = center;
            b = 0.5f * (w_ + e);
            r = 0.5f * (n + s);
            break;
        default: // B site
            b = center;
            g = 0.25f * (n + s + w_ + e);
            r = 0.25f * (nw + ne + sw + se);
            break;
    }

    // Black-level subtract & white-level normalize to [0,1].
    float bl = (float)P.blackLevel;
    r = (r - bl) * P.whiteScale;
    g = (g - bl) * P.whiteScale;
    b = (b - bl) * P.whiteScale;

    // White balance + gain.
    r *= P.wbR * P.gainR;
    g *= P.wbG * P.gainG;
    b *= P.wbB * P.gainB;

    // Camera RGB → sRGB color matrix.
    float Rs = P.m00 * r + P.m01 * g + P.m02 * b;
    float Gs = P.m10 * r + P.m11 * g + P.m12 * b;
    float Bs = P.m20 * r + P.m21 * g + P.m22 * b;

    // Gamma to display.
    r = linear_to_gamma(Rs);
    g = linear_to_gamma(Gs);
    b = linear_to_gamma(Bs);

    // Write in conceptual RGBA order — Metal's BGRA8Unorm storage format
    // handles the byte-order swap internally. Writing float4(b,g,r,1) here
    // would put our R value into the buffer's B channel and vice versa.
    out.write(float4(r, g, b, 1.0f), gid);
}
