// fused_ane_silu_proj_res.metal — SiLU + Conv1x1(2C→C) + Residual.
//
// F_ane MLP-branch second half. mlp1 (BN2-folded) has already produced a
// 2C-channel tensor. This kernel applies SiLU element-wise on all 2C
// channels, then projects 2C → C via mlp2, and adds the residual.
//
// Layout:
//   in_2c:   [H, W, 2C]   fp16   output of fused_ane_conv1x1 (bn2+mlp1 folded)
//   pj_w:    [C, 2C]      fp16   mlp2 weight
//   pj_b:    [C]          fp16   mlp2 bias
//   res_in:  [H, W, C]    fp16   x going into the MLP branch
//   out:     [H, W, C]    fp16

#include <metal_stdlib>
using namespace metal;

constant uint C_PJ_IN  [[function_constant(0)]];  // = 2*C
constant uint C_OUT    [[function_constant(1)]];  // = C
constant uint W_DIM    [[function_constant(2)]];
constant uint H_DIM    [[function_constant(3)]];

inline float silu_f(float v) {
    return v * (1.0f / (1.0f + metal::exp(-v)));
}

kernel void fused_ane_silu_proj_res(
    device const half * __restrict__ in_2c   [[buffer(0)]],
    device const half * __restrict__ pj_w    [[buffer(1)]],
    device const half * __restrict__ pj_b    [[buffer(2)]],
    device const half * __restrict__ res_in  [[buffer(3)]],
    device       half * __restrict__ out     [[buffer(4)]],
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= W_DIM || gid.y >= H_DIM) return;

    const uint pix    = gid.y * W_DIM + gid.x;
    const uint in_off = pix * C_PJ_IN;
    const uint res_off = pix * C_OUT;
    const uint out_off = pix * C_OUT;

    float v[128];                          // 2C ≤ 128
    for (uint c = 0; c < C_PJ_IN; ++c) {
        v[c] = silu_f((float)in_2c[in_off + c]);
    }

    for (uint o = 0; o < C_OUT; ++o) {
        float acc = (float)pj_b[o];
        const uint wrow = o * C_PJ_IN;
        for (uint c = 0; c < C_PJ_IN; ++c) {
            acc = fma((float)pj_w[wrow + c], v[c], acc);
        }
        acc += (float)res_in[res_off + o];
        out[out_off + o] = (half)acc;
    }
}
