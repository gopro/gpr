// Downscale.metal — bilinear BGRA8 → BGRA8 downscale for the Metal demosaic
// path. Demosaic writes into an intermediate CVPixelBuffer at bayer dims, this
// kernel reads from it and writes into the output CVPixelBuffer at a smaller
// (target) res.
//
// Bilinear sampling via the texture sampler keeps the kernel ~10 lines. For
// 8K→UHD this is good enough quality for a draft pipeline; if you ever need
// sharper output, swap to Lanczos or use MPSImageLanczosScale.

#include <metal_stdlib>
using namespace metal;

struct DownscaleParams {
    uint inW;
    uint inH;
    uint outW;
    uint outH;
};

kernel void downscale_bilinear_bgra8(
    texture2d<float, access::sample> inTex   [[texture(0)]],
    texture2d<float, access::write>  outTex  [[texture(1)]],
    constant DownscaleParams &P              [[buffer(0)]],
    uint2 gid                                 [[thread_position_in_grid]])
{
    if (gid.x >= P.outW || gid.y >= P.outH) return;

    // Map output pixel center → input UV (normalized).
    float u = ((float)gid.x + 0.5f) / (float)P.outW;
    float v = ((float)gid.y + 0.5f) / (float)P.outH;

    constexpr sampler s(coord::normalized,
                        address::clamp_to_edge,
                        filter::linear);
    float4 c = inTex.sample(s, float2(u, v));
    outTex.write(c, gid);
}
