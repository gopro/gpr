// fused_ane_conv1x1.metal — pure Conv1x1 (c → 2c).
//
// For F_ane (BatchNorm + SiLU) the BN is FOLDED into the following Conv1x1
// at weight-extraction time, so the kernel doesn't need to compute any norm.
// This is the replacement for fused_ln_conv1x1 in the legacy LN+SimpleGate
// path. Used for both bn1+conv1 and bn2+mlp1 expansions.
//
// Layout: NHWC channels-last, fp16.
//   in:     [H, W, C_in]      fp16
//   weight: [C_out, C_in]     fp16   (row-major, out-major)
//   bias:   [C_out]           fp16
//   out:    [H, W, C_out]     fp16
//
// One thread = one (h, w) pixel. fp32 accumulators inside.

#include <metal_stdlib>
using namespace metal;

constant uint C_IN  [[function_constant(0)]];
constant uint C_OUT [[function_constant(1)]];
constant uint W_DIM [[function_constant(2)]];
constant uint H_DIM [[function_constant(3)]];

kernel void fused_ane_conv1x1(
    device const half * __restrict__ in     [[buffer(0)]],
    device const half * __restrict__ wmat   [[buffer(1)]],   // [C_out, C_in]
    device const half * __restrict__ bias   [[buffer(2)]],
    device       half * __restrict__ out    [[buffer(3)]],
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= W_DIM || gid.y >= H_DIM) return;
    const uint pix = gid.y * W_DIM + gid.x;
    const uint in_off  = pix * C_IN;
    const uint out_off = pix * C_OUT;

    // Load input vector into registers.
    float v[128];   // C_IN ≤ 128 (covers w=16/32/64 + middle w=128 case).
    for (uint c = 0; c < C_IN; ++c) v[c] = (float)in[in_off + c];

    // Conv1x1 matmul: out[o] = bias[o] + sum_c wmat[o*C_IN + c] * v[c]
    for (uint o = 0; o < C_OUT; ++o) {
        float acc = (float)bias[o];
        const uint wrow = o * C_IN;
        for (uint c = 0; c < C_IN; ++c) {
            acc = fma((float)wmat[wrow + c], v[c], acc);
        }
        out[out_off + o] = (half)acc;
    }
}
