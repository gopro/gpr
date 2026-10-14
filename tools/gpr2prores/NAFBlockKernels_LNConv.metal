// fused_ln_conv1x1.metal — LayerNorm2d (channel-wise) fused with Conv2d 1x1 (c_in -> c_out).
//
// Layout: NHWC channels-last, fp16.
//   in:     [H, W, C_in]   fp16
//   gamma:  [C_in]         fp16  (LN affine scale)
//   beta:   [C_in]         fp16  (LN affine bias)
//   weight: [C_out, C_in]  fp16  (conv1x1 weight; out-major)
//   bias:   [C_out]        fp16  (conv1x1 bias; may be zeros if biasless)
//   out:    [H, W, C_out]  fp16
//
// One thread = one (h, w) pixel. C_in must be a multiple of 4 (we use half4 loads).
// C_in/C_out passed as constants. eps fixed at 1e-6 (fp32 accumulators for stability).
//
// This eliminates the LN intermediate entirely: LN output never touches device memory.

#include <metal_stdlib>
using namespace metal;

// Specialize for the two shapes we need in NAFBlock:
//   (a) LN(c=32)+Conv1x1(32->64)   -- norm1+conv1, mlp1
//   (b) LN(c=32)+Conv1x1(32->32)   -- not actually used; LN feeds 32->64 always
// We write a single templated-by-constant kernel and instantiate by C_in/C_out via [[function_constant]].

constant uint  C_IN  [[function_constant(0)]];
constant uint  C_OUT [[function_constant(1)]];
constant uint  W_DIM [[function_constant(2)]];
constant uint  H_DIM [[function_constant(3)]];

kernel void fused_ln_conv1x1(
    device const half * __restrict__ in     [[buffer(0)]],
    device const half * __restrict__ gamma  [[buffer(1)]],
    device const half * __restrict__ beta   [[buffer(2)]],
    device const half * __restrict__ wmat   [[buffer(3)]],  // [C_out, C_in] row-major
    device const half * __restrict__ bias   [[buffer(4)]],
    device       half * __restrict__ out    [[buffer(5)]],
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= W_DIM || gid.y >= H_DIM) return;
    const uint pix = gid.y * W_DIM + gid.x;
    const uint in_off  = pix * C_IN;
    const uint out_off = pix * C_OUT;

    // -- Pass 1: read input, accumulate mean & var (fp32 for stability) --
    // For C_IN <= 64 this fits easily in registers as a local array.
    // C_IN is known at JIT time so this loop is fully unrolled.
    float v[128];   // upper bound; if C_IN < 128, only first C_IN entries used.
    float sum = 0.0f, sumsq = 0.0f;
    for (uint c = 0; c < C_IN; ++c) {
        float x = (float)in[in_off + c];
        v[c] = x;
        sum   += x;
        sumsq += x * x;
    }
    float invN  = 1.0f / (float)C_IN;
    float mean  = sum * invN;
    float var   = fma(sumsq, invN, -mean * mean);
    float rstd  = rsqrt(var + 1e-6f);

    // -- Pass 2: in-register normalize + affine, then conv1x1 matmul --
    // Normalized values stored back into v[] (still fp32).
    for (uint c = 0; c < C_IN; ++c) {
        float g = (float)gamma[c];
        float b = (float)beta[c];
        v[c] = (v[c] - mean) * rstd * g + b;
    }

    // Conv1x1: out[o] = bias[o] + sum_c wmat[o*C_IN + c] * v[c].
    // wmat is row-major [C_OUT, C_IN] so out-row is contiguous in memory.
    for (uint o = 0; o < C_OUT; ++o) {
        float acc = (float)bias[o];
        const uint wrow = o * C_IN;
        for (uint c = 0; c < C_IN; ++c) {
            acc = fma((float)wmat[wrow + c], v[c], acc);
        }
        out[out_off + o] = (half)acc;
    }
}
