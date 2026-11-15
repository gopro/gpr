// fused_ane_dw_silu_proj_res.metal — DW(k×k) + SiLU + Conv1x1(2C→C) + Residual.
//
// F_ane attention-branch second half. k is a function constant (3 or 7).
// SimpleGate (chunk+multiply halving) is REPLACED by SiLU (x · sigmoid(x))
// applied element-wise on all 2C channels — the channel-halving moves to
// proj1 which is now Conv1x1(2C → C).
//
// Layout:
//   in_2c:   [H, W, 2C]   fp16   output of fused_ane_conv1x1 (bn1+conv1 folded)
//   dw_w:    [2C, k*k]    fp16
//   dw_b:    [2C]         fp16
//   pj_w:    [C, 2C]      fp16
//   pj_b:    [C]          fp16
//   res_in:  [H, W, C]    fp16   block input x (the residual source)
//   out:     [H, W, C]    fp16
//
// One thread = one (h, w) pixel. fp32 accumulators.
// Boundary handling: zero-pad (matches PyTorch Conv2d default padding mode).

#include <metal_stdlib>
using namespace metal;

constant uint C_PJ_IN  [[function_constant(0)]];  // = 2 * C
constant uint C_OUT    [[function_constant(1)]];  // = C
constant uint W_DIM    [[function_constant(2)]];
constant uint H_DIM    [[function_constant(3)]];
constant uint DW_K     [[function_constant(4)]];  // 3 or 7

inline float silu_f(float v) {
    // x * sigmoid(x)  =  x / (1 + exp(-x))
    return v * (1.0f / (1.0f + metal::exp(-v)));
}

kernel void fused_ane_dw_silu_proj_res(
    device const half * __restrict__ in_2c   [[buffer(0)]],
    device const half * __restrict__ dw_w    [[buffer(1)]],
    device const half * __restrict__ dw_b    [[buffer(2)]],
    device const half * __restrict__ pj_w    [[buffer(3)]],
    device const half * __restrict__ pj_b    [[buffer(4)]],
    device const half * __restrict__ res_in  [[buffer(5)]],
    device       half * __restrict__ out     [[buffer(6)]],
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= W_DIM || gid.y >= H_DIM) return;

    const uint x = gid.x;
    const uint y = gid.y;
    const uint pad = DW_K / 2;
    const uint c_pj_in = C_PJ_IN;          // = 2*C
    const uint c_out   = C_OUT;            // = C

    // DW conv + bias + SiLU, producing v[c] for c in [0, 2C).
    // Then matmul: out_c = pj_b[c] + Σ_i pj_w[c, i] * v[i] + res[c].
    //
    // Accumulator scratch lives in thread-local memory.
    float v[128];   // 2C ≤ 128  (covers w∈{16,32,64} blocks + middle 2C=256 needs split).

    for (uint c = 0; c < c_pj_in; ++c) {
        float acc = (float)dw_b[c];
        // DW kernel iterates over k×k window for this channel only.
        for (uint dy = 0; dy < DW_K; ++dy) {
            int yy = (int)y + (int)dy - (int)pad;
            if (yy < 0 || yy >= (int)H_DIM) continue;
            for (uint dx = 0; dx < DW_K; ++dx) {
                int xx = (int)x + (int)dx - (int)pad;
                if (xx < 0 || xx >= (int)W_DIM) continue;
                const uint in_off = ((uint)yy * W_DIM + (uint)xx) * c_pj_in + c;
                const uint w_off  = c * (DW_K * DW_K) + dy * DW_K + dx;
                acc = fma((float)dw_w[w_off], (float)in_2c[in_off], acc);
            }
        }
        v[c] = silu_f(acc);
    }

    // proj1 matmul + bias + residual
    const uint pix_out = (y * W_DIM + x) * c_out;
    const uint pix_res = (y * W_DIM + x) * c_out;
    for (uint o = 0; o < c_out; ++o) {
        float acc = (float)pj_b[o];
        const uint wrow = o * c_pj_in;
        for (uint c = 0; c < c_pj_in; ++c) {
            acc = fma((float)pj_w[wrow + c], v[c], acc);
        }
        acc += (float)res_in[pix_res + o];
        out[pix_out + o] = (half)acc;
    }
}
