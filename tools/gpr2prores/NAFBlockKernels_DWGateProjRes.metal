// fused_dw_gate_proj_res.metal
// DW3x3 -> SimpleGate -> Conv1x1(C->C) -> residual add
// Output: out = xres + proj(gate(dw(x_2c)))

#include <metal_stdlib>
using namespace metal;

constant uint C       [[function_constant(0)]];   // gated-half channel count
constant uint W_DIM   [[function_constant(1)]];
constant uint H_DIM   [[function_constant(2)]];

kernel void fused_dw_gate_proj_res(
    device const half * __restrict__ x      [[buffer(0)]],   // [H,W,2C] pre-DW features
    device const half * __restrict__ xres   [[buffer(1)]],   // [H,W,C]  block input for residual
    device const half * __restrict__ dw_w   [[buffer(2)]],   // [2C,3,3]
    device const half * __restrict__ dw_b   [[buffer(3)]],   // [2C]
    device const half * __restrict__ pj_w   [[buffer(4)]],   // [C,C]
    device const half * __restrict__ pj_b   [[buffer(5)]],   // [C]
    device       half * __restrict__ out    [[buffer(6)]],   // [H,W,C]
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= W_DIM || gid.y >= H_DIM) return;
    const uint h = gid.y, w = gid.x;
    const uint TWOC = 2 * C;

    float dw_acc[256];   // supports C up to 128
    for (uint c2 = 0; c2 < TWOC; ++c2) dw_acc[c2] = (float)dw_b[c2];

    for (int ky = -1; ky <= 1; ++ky) {
        int hh = (int)h + ky;
        if (hh < 0 || hh >= (int)H_DIM) continue;
        for (int kx = -1; kx <= 1; ++kx) {
            int ww = (int)w + kx;
            if (ww < 0 || ww >= (int)W_DIM) continue;
            const uint pix_off = ((uint)hh * W_DIM + (uint)ww) * TWOC;
            const uint k_off   = ((uint)(ky + 1) * 3 + (uint)(kx + 1));
            for (uint c2 = 0; c2 < TWOC; ++c2) {
                float wv = (float)dw_w[c2 * 9 + k_off];
                float xv = (float)x[pix_off + c2];
                dw_acc[c2] = fma(wv, xv, dw_acc[c2]);
            }
        }
    }

    float gate[128];     // supports C up to 128
    for (uint c = 0; c < C; ++c) {
        gate[c] = dw_acc[c] * dw_acc[c + C];
    }

    const uint out_off = (h * W_DIM + w) * C;
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
