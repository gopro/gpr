// fused_gate_proj_res.metal — SimpleGate + Conv1x1(C->C) + residual.
//
// One thread = one (h,w) pixel. NHWC fp16.
//   in    : [H, W, 2C]   pre-gate features (= mlp1(LN(x)) output)
//   xres  : [H, W, C]    pre-half2 input for residual
//   pj_w  : [C, C]       conv weight, row-major [out, in]
//   pj_b  : [C]
//   out   : [H, W, C]
//
// out[o] = xres[o] + pj_b[o] + sum_c pj_w[o,c] * (in[c] * in[c+C])

#include <metal_stdlib>
using namespace metal;

constant uint C       [[function_constant(0)]];
constant uint W_DIM   [[function_constant(1)]];
constant uint H_DIM   [[function_constant(2)]];

kernel void fused_gate_proj_res(
    device const half * __restrict__ in     [[buffer(0)]],   // [H,W,2C]
    device const half * __restrict__ xres   [[buffer(1)]],   // [H,W,C]
    device const half * __restrict__ pj_w   [[buffer(2)]],   // [C,C]
    device const half * __restrict__ pj_b   [[buffer(3)]],   // [C]
    device       half * __restrict__ out    [[buffer(4)]],   // [H,W,C]
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= W_DIM || gid.y >= H_DIM) return;
    const uint pix = gid.y * W_DIM + gid.x;
    const uint in_off = pix * (2 * C);
    const uint out_off = pix * C;

    // SimpleGate into gate[c], up to C=128
    float gate[128];
    for (uint c = 0; c < C; ++c) {
        float a = (float)in[in_off + c];
        float b = (float)in[in_off + c + C];
        gate[c] = a * b;
    }

    // Conv1x1 + residual
    for (uint o = 0; o < C; ++o) {
        float acc = (float)pj_b[o];
        const uint wrow = o * C;
        for (uint c = 0; c < C; ++c) {
            acc = fma((float)pj_w[wrow + c], gate[c], acc);
        }
        float r = (float)xres[out_off + o];
        out[out_off + o] = (half)(acc + r);
    }
}
