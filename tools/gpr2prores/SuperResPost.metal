// SuperResPost.metal — post-processing kernels for SuperResMetal.
//
// 1) bicubic_2x_4chan: 2x bicubic upsample for NHWC fp16 4-channel planes.
//    Input  : [Hp, Wp, 4] fp16  (zero-padded planes)
//    Output : [2Hp, 2Wp, 4] fp16  (bicubic baseline)
//    Matches torch.nn.functional.interpolate(mode='bicubic', align_corners=False).
//
// 2) combine_rebayer: combine bicubic baseline + scaled residual, clamp, and
//    write back to a Bayer uint16 grid cropped to (outW, outH).
//    out_pixel = clamp(baseline + 0.01 * residual, 0, 1) * 16383
//    Bayer layout: row 2y has [R, Gr]; row 2y+1 has [Gb, B] (CFA=0, RGGB).

#include <metal_stdlib>
using namespace metal;

constant float RES_SCALE = 0.01f;
constant float WHITE = 16383.0f;

// Catmull-Rom cubic.
static inline float cubic_f(float a, float b, float c, float d, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * (
        (2.0f * b) +
        (-a + c) * t +
        (2.0f * a - 5.0f * b + 4.0f * c - d) * t2 +
        (-a + 3.0f * b - 3.0f * c + d) * t3
    );
}

static inline int clampi(int v, int hi) { return v < 0 ? 0 : (v >= hi ? hi - 1 : v); }

// ---- Kernel 1: 2x bicubic of NHWC fp16 4-channel ----
// gid = (ox, oy), output of size (2Wp, 2Hp).
// Per output pixel, we read a 4x4 neighborhood from input and produce 4 channels.
kernel void bicubic_2x_4chan(
    device const half * __restrict__ in   [[buffer(0)]],   // [Hp, Wp, 4]
    device       half * __restrict__ out  [[buffer(1)]],   // [2Hp, 2Wp, 4]
    constant uint &Wp                       [[buffer(2)]],
    constant uint &Hp                       [[buffer(3)]],
    uint2 gid                                [[thread_position_in_grid]])
{
    const uint OW = 2u * Wp;
    const uint OH = 2u * Hp;
    if (gid.x >= OW || gid.y >= OH) return;
    int ox = (int)gid.x;
    int oy = (int)gid.y;
    float src_y = ((float)oy + 0.5f) * 0.5f - 0.5f;
    float src_x = ((float)ox + 0.5f) * 0.5f - 0.5f;
    int iy = (int)floor(src_y);
    int ix = (int)floor(src_x);
    float fy = src_y - (float)iy;
    float fx = src_x - (float)ix;

    // Sample the 4x4 neighborhood for all 4 channels at once.
    // We process each channel independently.
    float r[4], g1[4], g2[4], b[4];
    for (int dy = -1; dy <= 2; ++dy) {
        int y = clampi(iy + dy, (int)Hp);
        int xs[4];
        xs[0] = clampi(ix - 1, (int)Wp);
        xs[1] = clampi(ix    , (int)Wp);
        xs[2] = clampi(ix + 1, (int)Wp);
        xs[3] = clampi(ix + 2, (int)Wp);
        float aR, bR, cR, dR;
        float aG1, bG1, cG1, dG1;
        float aG2, bG2, cG2, dG2;
        float aB, bB, cB, dB;
        uint p0 = (uint)y * Wp * 4u + (uint)xs[0] * 4u;
        uint p1 = (uint)y * Wp * 4u + (uint)xs[1] * 4u;
        uint p2 = (uint)y * Wp * 4u + (uint)xs[2] * 4u;
        uint p3 = (uint)y * Wp * 4u + (uint)xs[3] * 4u;
        aR = (float)in[p0+0]; bR = (float)in[p1+0]; cR = (float)in[p2+0]; dR = (float)in[p3+0];
        aG1 = (float)in[p0+1]; bG1 = (float)in[p1+1]; cG1 = (float)in[p2+1]; dG1 = (float)in[p3+1];
        aG2 = (float)in[p0+2]; bG2 = (float)in[p1+2]; cG2 = (float)in[p2+2]; dG2 = (float)in[p3+2];
        aB = (float)in[p0+3]; bB = (float)in[p1+3]; cB = (float)in[p2+3]; dB = (float)in[p3+3];
        r [dy+1] = cubic_f(aR, bR, cR, dR, fx);
        g1[dy+1] = cubic_f(aG1, bG1, cG1, dG1, fx);
        g2[dy+1] = cubic_f(aG2, bG2, cG2, dG2, fx);
        b [dy+1] = cubic_f(aB, bB, cB, dB, fx);
    }
    float vR  = cubic_f(r[0],  r[1],  r[2],  r[3],  fy);
    float vG1 = cubic_f(g1[0], g1[1], g1[2], g1[3], fy);
    float vG2 = cubic_f(g2[0], g2[1], g2[2], g2[3], fy);
    float vB  = cubic_f(b[0],  b[1],  b[2],  b[3],  fy);
    uint o = (uint)oy * OW * 4u + (uint)ox * 4u;
    out[o+0] = (half)vR;
    out[o+1] = (half)vG1;
    out[o+2] = (half)vG2;
    out[o+3] = (half)vB;
}

// ---- Kernel 2: combine bicubic + residual + re-bayer + clamp ----
// gid = (x_planes, y_planes) in plane coordinates (each thread writes 2x2 Bayer).
// outW_pl, outH_pl are crop dims in plane units (outW/2, outH/2).
// (out_Wpp, out_Hpp) are bicubic/residual buffer dims = 2*Wp x 2*Hp.
// outStridePix is the destination Bayer row stride in pixels (uint16 units).
// For tightly packed buffers this == outW; for IOSurface-backed CVPixelBuffers
// the row stride is padded to >=64-byte alignment by CoreVideo so it may
// exceed outW. We honor whatever stride the caller passes.
kernel void combine_rebayer(
    device const half * __restrict__ baseline  [[buffer(0)]],   // [out_Hpp, out_Wpp, 4]
    device const half * __restrict__ residual  [[buffer(1)]],   // [out_Hpp, out_Wpp, 4]
    device       ushort * __restrict__ bayer   [[buffer(2)]],   // [outH, outStridePix] uint16
    constant uint &out_Wpp                       [[buffer(3)]],
    constant uint &out_Hpp                       [[buffer(4)]],
    constant uint &outW                          [[buffer(5)]],
    constant uint &outH                          [[buffer(6)]],
    constant uint &outStridePix                  [[buffer(7)]],
    uint2 gid                                    [[thread_position_in_grid]])
{
    uint x = gid.x, y = gid.y;
    uint outW_pl = outW / 2;
    uint outH_pl = outH / 2;
    if (x >= outW_pl || y >= outH_pl) return;
    uint idx = (y * out_Wpp + x) * 4u;
    float r  = (float)baseline[idx+0] + RES_SCALE * (float)residual[idx+0];
    float g1 = (float)baseline[idx+1] + RES_SCALE * (float)residual[idx+1];
    float g2 = (float)baseline[idx+2] + RES_SCALE * (float)residual[idx+2];
    float b  = (float)baseline[idx+3] + RES_SCALE * (float)residual[idx+3];
    r  = clamp(r,  0.0f, 1.0f);
    g1 = clamp(g1, 0.0f, 1.0f);
    g2 = clamp(g2, 0.0f, 1.0f);
    b  = clamp(b,  0.0f, 1.0f);
    uint by0 = 2u * y;
    uint by1 = by0 + 1u;
    uint bx0 = 2u * x;
    uint bx1 = bx0 + 1u;
    bayer[by0 * outStridePix + bx0] = (ushort)(r  * WHITE);
    bayer[by0 * outStridePix + bx1] = (ushort)(g1 * WHITE);
    bayer[by1 * outStridePix + bx0] = (ushort)(g2 * WHITE);
    bayer[by1 * outStridePix + bx1] = (ushort)(b  * WHITE);
}

// ---- Kernel 2b (FUSED): bicubic + combine + clamp + rebayer in one pass ----
// Writes the final Bayer uint16 directly from the input planes (Hpp, Wpp, 4)
// fp16 and CNN residual (out_Hpp, out_Wpp, 4) fp16 — never materializes the
// (out_Hpp × out_Wpp × 4 × 2)-byte baseline buffer. For 8280×5520 Bayer this
// saves ~45 MB of write+read traffic.
//
// gid = (x_pl, y_pl) in OUTPUT plane coordinates (each thread writes a 2x2
// Bayer block). Bicubic upsamples from (Hpp, Wpp) -> (2Hp, 2Wp) at integer
// plane-output coord (y, x); the kernel samples the 4-channel input plane
// neighbors and returns 4 channel values for this (y, x).
//
// out_Wpp/out_Hpp are the bicubic/residual buffer dims = (2*Wp, 2*Hp).
// outW/outH are the Bayer crop dims (typically 2*outW_pl, 2*outH_pl).
// Wp, Hp are the input plane dims (the un-doubled plane size that the
// bicubic reads from). Wpp = padded input plane width = out_Wpp/2.
kernel void bicubic_combine_rebayer(
    device const half * __restrict__ in        [[buffer(0)]],   // [Hpp, Wpp, 4]
    device const half * __restrict__ residual  [[buffer(1)]],   // [out_Hpp, out_Wpp, 4]
    device       ushort * __restrict__ bayer   [[buffer(2)]],   // [outH, outStridePix] uint16
    constant uint &Wpp                          [[buffer(3)]],   // padded plane W
    constant uint &Hpp                          [[buffer(4)]],   // padded plane H
    constant uint &out_Wpp                      [[buffer(5)]],
    constant uint &out_Hpp                      [[buffer(6)]],
    constant uint &outW                         [[buffer(7)]],
    constant uint &outH                         [[buffer(8)]],
    constant uint &outStridePix                 [[buffer(9)]],
    uint2 gid                                   [[thread_position_in_grid]])
{
    uint x = gid.x, y = gid.y;
    uint outW_pl = outW / 2;
    uint outH_pl = outH / 2;
    if (x >= outW_pl || y >= outH_pl) return;

    // Bicubic at output-plane coord (y, x). The original bicubic_2x_4chan
    // kernel maps output coord (ox, oy) in [0..out_Wpp) to source coord
    //   src = ((o + 0.5) * 0.5 - 0.5)
    // We use the same formula at (x, y) (so we land at the same lattice
    // points as the un-fused path for byte-identical output).
    float src_y = ((float)y + 0.5f) * 0.5f - 0.5f;
    float src_x = ((float)x + 0.5f) * 0.5f - 0.5f;
    int iy = (int)floor(src_y);
    int ix = (int)floor(src_x);
    float fy = src_y - (float)iy;
    float fx = src_x - (float)ix;

    // Sample 4x4 neighborhood for the 4 channels (kept in thread registers).
    float r4[4], g14[4], g24[4], b4[4];
    for (int dy = -1; dy <= 2; ++dy) {
        int yy = clampi(iy + dy, (int)Hpp);
        int xs[4];
        xs[0] = clampi(ix - 1, (int)Wpp);
        xs[1] = clampi(ix    , (int)Wpp);
        xs[2] = clampi(ix + 1, (int)Wpp);
        xs[3] = clampi(ix + 2, (int)Wpp);
        uint p0 = (uint)yy * Wpp * 4u + (uint)xs[0] * 4u;
        uint p1 = (uint)yy * Wpp * 4u + (uint)xs[1] * 4u;
        uint p2 = (uint)yy * Wpp * 4u + (uint)xs[2] * 4u;
        uint p3 = (uint)yy * Wpp * 4u + (uint)xs[3] * 4u;
        float aR  = (float)in[p0+0], bR_  = (float)in[p1+0], cR  = (float)in[p2+0], dR  = (float)in[p3+0];
        float aG1 = (float)in[p0+1], bG1  = (float)in[p1+1], cG1 = (float)in[p2+1], dG1 = (float)in[p3+1];
        float aG2 = (float)in[p0+2], bG2  = (float)in[p1+2], cG2 = (float)in[p2+2], dG2 = (float)in[p3+2];
        float aB  = (float)in[p0+3], bB_  = (float)in[p1+3], cB  = (float)in[p2+3], dB  = (float)in[p3+3];
        r4 [dy+1] = cubic_f(aR,  bR_, cR,  dR,  fx);
        g14[dy+1] = cubic_f(aG1, bG1, cG1, dG1, fx);
        g24[dy+1] = cubic_f(aG2, bG2, cG2, dG2, fx);
        b4 [dy+1] = cubic_f(aB,  bB_, cB,  dB,  fx);
    }
    // Cast to half then back to float, to match the un-fused path which stores
    // the bicubic result as half and re-loads it. Without this round-trip the
    // fused output could differ by 1 LSB.
    float vR  = (float)(half)cubic_f(r4 [0], r4 [1], r4 [2], r4 [3], fy);
    float vG1 = (float)(half)cubic_f(g14[0], g14[1], g14[2], g14[3], fy);
    float vG2 = (float)(half)cubic_f(g24[0], g24[1], g24[2], g24[3], fy);
    float vB  = (float)(half)cubic_f(b4 [0], b4 [1], b4 [2], b4 [3], fy);

    // Combine + clamp + rebayer.
    uint ridx = (y * out_Wpp + x) * 4u;
    float r  = vR  + RES_SCALE * (float)residual[ridx+0];
    float g1 = vG1 + RES_SCALE * (float)residual[ridx+1];
    float g2 = vG2 + RES_SCALE * (float)residual[ridx+2];
    float b  = vB  + RES_SCALE * (float)residual[ridx+3];
    r  = clamp(r,  0.0f, 1.0f);
    g1 = clamp(g1, 0.0f, 1.0f);
    g2 = clamp(g2, 0.0f, 1.0f);
    b  = clamp(b,  0.0f, 1.0f);
    uint by0 = 2u * y;
    uint by1 = by0 + 1u;
    uint bx0 = 2u * x;
    uint bx1 = bx0 + 1u;
    bayer[by0 * outStridePix + bx0] = (ushort)(r  * WHITE);
    bayer[by0 * outStridePix + bx1] = (ushort)(g1 * WHITE);
    bayer[by1 * outStridePix + bx0] = (ushort)(g2 * WHITE);
    bayer[by1 * outStridePix + bx1] = (ushort)(b  * WHITE);
}

// ---- Kernel 2c (FUSED, 1x mode): combine input + residual + rebayer ----
// For the BIBO_1x (F_no_sr) variant: no bicubic baseline, no PixelShuffle SR
// head. The CNN outputs cleaned planes at the SAME dims as the input planes.
// We just add `RES_SCALE * residual` to the input, clamp, and rebayer.
//
// Input planes:  (Hpp, Wpp, 4) fp16 — same buffer that fed the CNN
// Residual:      (Hpp, Wpp, 4) fp16 — CNN output (1x mode)
// Output bayer:  (outH, outW) uint16; outH=2*outH_pl, outW=2*outW_pl.
// Each thread writes a 2x2 Bayer cell from one plane coord (x_pl, y_pl).
kernel void combine_rebayer_1x(
    device const half * __restrict__ in        [[buffer(0)]],   // [Hpp, Wpp, 4]
    device const half * __restrict__ residual  [[buffer(1)]],   // [Hpp, Wpp, 4]
    device       ushort * __restrict__ bayer   [[buffer(2)]],   // [outH, outStridePix] uint16
    constant uint &Wpp                          [[buffer(3)]],   // padded plane W
    constant uint &Hpp                          [[buffer(4)]],   // padded plane H
    constant uint &outW                         [[buffer(5)]],
    constant uint &outH                         [[buffer(6)]],
    constant uint &outStridePix                 [[buffer(7)]],
    uint2 gid                                   [[thread_position_in_grid]])
{
    uint x = gid.x, y = gid.y;
    uint outW_pl = outW / 2;
    uint outH_pl = outH / 2;
    if (x >= outW_pl || y >= outH_pl) return;
    if (x >= Hpp || y >= Wpp) {} // suppress unused-warn for Hpp
    uint idx = (y * Wpp + x) * 4u;
    float r  = (float)in[idx+0] + RES_SCALE * (float)residual[idx+0];
    float g1 = (float)in[idx+1] + RES_SCALE * (float)residual[idx+1];
    float g2 = (float)in[idx+2] + RES_SCALE * (float)residual[idx+2];
    float b  = (float)in[idx+3] + RES_SCALE * (float)residual[idx+3];
    r  = clamp(r,  0.0f, 1.0f);
    g1 = clamp(g1, 0.0f, 1.0f);
    g2 = clamp(g2, 0.0f, 1.0f);
    b  = clamp(b,  0.0f, 1.0f);
    uint by0 = 2u * y;
    uint by1 = by0 + 1u;
    uint bx0 = 2u * x;
    uint bx1 = bx0 + 1u;
    bayer[by0 * outStridePix + bx0] = (ushort)(r  * WHITE);
    bayer[by0 * outStridePix + bx1] = (ushort)(g1 * WHITE);
    bayer[by1 * outStridePix + bx0] = (ushort)(g2 * WHITE);
    bayer[by1 * outStridePix + bx1] = (ushort)(b  * WHITE);
}

// ---- Kernel 3: pack Bayer uint16 -> NHWC fp16 (4-plane, normalized [0,1]) ----
// gid = (x_pl, y_pl). plane coords; reads 2x2 bayer pixels per thread.
// Writes [Hp, Wp, 4] fp16, where Hp = inH/2, Wp = inW/2, and the result is
// **padded** to (Hpp, Wpp) — the kernel takes Hpp/Wpp as the destination
// stride. The padded region is left zeroed by the host (we only dispatch
// (Wp, Hp) threads).
kernel void unpack_bayer_to_nhwc4(
    device const ushort * __restrict__ bayer   [[buffer(0)]],   // [inH, inW]
    device       half  * __restrict__ planes   [[buffer(1)]],   // [Hpp, Wpp, 4]
    constant uint &inW                          [[buffer(2)]],
    constant uint &Wpp                          [[buffer(3)]],
    constant uint &Wp                           [[buffer(4)]],
    constant uint &Hp                           [[buffer(5)]],
    uint2 gid                                   [[thread_position_in_grid]])
{
    uint x = gid.x, y = gid.y;
    if (x >= Wp || y >= Hp) return;
    uint by0 = 2u * y;
    uint by1 = by0 + 1u;
    uint bx0 = 2u * x;
    uint bx1 = bx0 + 1u;
    float r  = (float)bayer[by0 * inW + bx0] * (1.0f / 16383.0f);
    float g1 = (float)bayer[by0 * inW + bx1] * (1.0f / 16383.0f);
    float g2 = (float)bayer[by1 * inW + bx0] * (1.0f / 16383.0f);
    float b  = (float)bayer[by1 * inW + bx1] * (1.0f / 16383.0f);
    uint idx = (y * Wpp + x) * 4u;
    planes[idx+0] = (half)r;
    planes[idx+1] = (half)g1;
    planes[idx+2] = (half)g2;
    planes[idx+3] = (half)b;
}
