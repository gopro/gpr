// test_F_ane_kernels.m — standalone harness for the 3 new F_ane Metal kernels.
//
// Loads Demosaic.metallib (built by the Makefile), runs each kernel with
// hand-crafted inputs, and compares against a CPU fp32 reference.
//
// Pass iff every per-pixel diff < 0.05 absolute (loose because outputs are
// in fp16 and Conv1x1 of medium-sized vectors accumulates a few ulp).
//
// Build:
//   clang -fobjc-arc -O2 -fmodules \
//     -framework Foundation -framework Metal \
//     test_F_ane_kernels.m -o test_F_ane_kernels
//
// Run from this directory so Demosaic.metallib is found alongside.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

// fp16 helpers (host-side conversion). Apple's _Float16 simplifies this.
static uint16_t f32_to_f16(float v) {
    _Float16 h = (_Float16)v;
    uint16_t u;
    memcpy(&u, &h, 2);
    return u;
}
static float f16_to_f32(uint16_t u) {
    _Float16 h;
    memcpy(&h, &u, 2);
    return (float)h;
}

// SiLU = x * sigmoid(x).
static float silu(float v) { return v / (1.0f + expf(-v)); }


// ---------- Reference CPU implementations ----------

// Conv1x1: out[h,w,o] = bias[o] + Σ_c W[o,c] * in[h,w,c]
static void cpu_conv1x1(const float *in, const float *W, const float *b, float *out,
                        int H, int W_dim, int Cin, int Cout)
{
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W_dim; ++x) {
            const float *iv = in + (y*W_dim + x) * Cin;
            float *ov = out + (y*W_dim + x) * Cout;
            for (int o = 0; o < Cout; ++o) {
                float acc = b[o];
                for (int c = 0; c < Cin; ++c) acc += W[o*Cin + c] * iv[c];
                ov[o] = acc;
            }
        }
    }
}

// DW(k×k) + bias, zero-pad: per-channel, separable channels.
static void cpu_dwconv(const float *in, const float *W, const float *b, float *out,
                       int H, int Wd, int C, int K)
{
    int pad = K/2;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < Wd; ++x) {
            for (int c = 0; c < C; ++c) {
                float acc = b[c];
                for (int dy = 0; dy < K; ++dy) {
                    int yy = y + dy - pad;
                    if (yy < 0 || yy >= H) continue;
                    for (int dx = 0; dx < K; ++dx) {
                        int xx = x + dx - pad;
                        if (xx < 0 || xx >= Wd) continue;
                        acc += W[c*K*K + dy*K + dx] * in[(yy*Wd + xx)*C + c];
                    }
                }
                out[(y*Wd + x)*C + c] = acc;
            }
        }
    }
}


// ---------- fp16 buffer helpers ----------

static id<MTLBuffer> makeBufFromFloats(id<MTLDevice> dev, const float *src, size_t n) {
    uint16_t *tmp = malloc(n * 2);
    for (size_t i = 0; i < n; ++i) tmp[i] = f32_to_f16(src[i]);
    id<MTLBuffer> buf = [dev newBufferWithBytes:tmp length:n*2 options:MTLResourceStorageModeShared];
    free(tmp);
    return buf;
}
static void bufToFloats(id<MTLBuffer> buf, float *dst, size_t n) {
    uint16_t *src = (uint16_t *)buf.contents;
    for (size_t i = 0; i < n; ++i) dst[i] = f16_to_f32(src[i]);
}


// ---------- PSO builder with function constants ----------

static id<MTLComputePipelineState> makePSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                            NSString *name,
                                            const uint32_t *consts, int n_consts)
{
    MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new];
    for (int i = 0; i < n_consts; ++i) {
        [fc setConstantValue:&consts[i] type:MTLDataTypeUInt atIndex:i];
    }
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:name constantValues:fc error:&err];
    if (!fn) { fprintf(stderr, "PSO[%s] fn err: %s\n", [name UTF8String], [[err localizedDescription] UTF8String]); return nil; }
    id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) { fprintf(stderr, "PSO[%s] pipeline err: %s\n", [name UTF8String], [[err localizedDescription] UTF8String]); return nil; }
    return pso;
}


// ---------- Tests ----------

static int test_conv1x1(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib)
{
    const int H = 8, Wd = 8, Cin = 16, Cout = 32;

    // Deterministic inputs.
    int Nin = H*Wd*Cin;
    int Nw  = Cout*Cin;
    int Nout = H*Wd*Cout;
    float *in = malloc(Nin*sizeof(float));
    float *Wt = malloc(Nw*sizeof(float));
    float *bi = malloc(Cout*sizeof(float));
    float *ref = malloc(Nout*sizeof(float));
    for (int i = 0; i < Nin; ++i) in[i] = sinf(0.1f*i) * 0.5f;
    for (int i = 0; i < Nw; ++i)  Wt[i] = cosf(0.07f*i) * 0.2f;
    for (int i = 0; i < Cout; ++i) bi[i] = (i-Cout/2) * 0.01f;

    cpu_conv1x1(in, Wt, bi, ref, H, Wd, Cin, Cout);

    id<MTLBuffer> bIn = makeBufFromFloats(dev, in, Nin);
    id<MTLBuffer> bW  = makeBufFromFloats(dev, Wt, Nw);
    id<MTLBuffer> bB  = makeBufFromFloats(dev, bi, Cout);
    id<MTLBuffer> bOut = [dev newBufferWithLength:Nout*2 options:MTLResourceStorageModeShared];

    uint32_t consts[] = {(uint32_t)Cin, (uint32_t)Cout, (uint32_t)Wd, (uint32_t)H};
    id<MTLComputePipelineState> pso = makePSO(dev, lib, @"fused_ane_conv1x1", consts, 4);
    if (!pso) return 1;

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ec = [cb computeCommandEncoder];
    [ec setComputePipelineState:pso];
    [ec setBuffer:bIn  offset:0 atIndex:0];
    [ec setBuffer:bW   offset:0 atIndex:1];
    [ec setBuffer:bB   offset:0 atIndex:2];
    [ec setBuffer:bOut offset:0 atIndex:3];
    MTLSize grid = MTLSizeMake(Wd, H, 1);
    MTLSize tg   = MTLSizeMake(8, 8, 1);
    [ec dispatchThreads:grid threadsPerThreadgroup:tg];
    [ec endEncoding];
    [cb commit]; [cb waitUntilCompleted];

    float *got = malloc(Nout*sizeof(float));
    bufToFloats(bOut, got, Nout);

    float max_diff = 0, mean_diff = 0;
    for (int i = 0; i < Nout; ++i) {
        float d = fabsf(ref[i] - got[i]);
        max_diff = fmaxf(max_diff, d);
        mean_diff += d;
    }
    mean_diff /= Nout;
    printf("  fused_ane_conv1x1     max_diff=%.5f  mean_diff=%.5f  ref[0]=%.4f got[0]=%.4f\n",
           max_diff, mean_diff, ref[0], got[0]);
    int ok = max_diff < 0.05f;
    free(in); free(Wt); free(bi); free(ref); free(got);
    return ok ? 0 : 1;
}

static int test_dw_silu_proj_res(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib,
                                  int DW_K)
{
    const int H = 8, Wd = 8, C = 16;
    const int C_pj_in = 2*C;
    int Ndw_in = H*Wd*C_pj_in;
    int Nres = H*Wd*C;
    int Ndw_w = C_pj_in * DW_K * DW_K;
    int Npj_w = C * C_pj_in;
    int Nout = H*Wd*C;

    float *in2c = malloc(Ndw_in*sizeof(float));
    float *dwW  = malloc(Ndw_w*sizeof(float));
    float *dwB  = malloc(C_pj_in*sizeof(float));
    float *pjW  = malloc(Npj_w*sizeof(float));
    float *pjB  = malloc(C*sizeof(float));
    float *res  = malloc(Nres*sizeof(float));
    for (int i = 0; i < Ndw_in; ++i) in2c[i] = sinf(0.13f*i) * 0.4f;
    for (int i = 0; i < Ndw_w; ++i) dwW[i] = cosf(0.05f*i) * 0.3f;
    for (int i = 0; i < C_pj_in; ++i) dwB[i] = 0.0f;
    for (int i = 0; i < Npj_w; ++i) pjW[i] = sinf(0.09f*i) * 0.15f;
    for (int i = 0; i < C; ++i) pjB[i] = 0.0f;
    for (int i = 0; i < Nres; ++i) res[i] = cosf(0.21f*i) * 0.3f;

    // CPU ref:  silu( dwconv(in2c) )  →  proj1(2C → C)  →  + res
    float *dwout = malloc(Ndw_in*sizeof(float));
    cpu_dwconv(in2c, dwW, dwB, dwout, H, Wd, C_pj_in, DW_K);
    for (int i = 0; i < Ndw_in; ++i) dwout[i] = silu(dwout[i]);
    float *ref = malloc(Nout*sizeof(float));
    cpu_conv1x1(dwout, pjW, pjB, ref, H, Wd, C_pj_in, C);
    for (int i = 0; i < Nout; ++i) ref[i] += res[i];

    // GPU
    id<MTLBuffer> bIn  = makeBufFromFloats(dev, in2c, Ndw_in);
    id<MTLBuffer> bDwW = makeBufFromFloats(dev, dwW, Ndw_w);
    id<MTLBuffer> bDwB = makeBufFromFloats(dev, dwB, C_pj_in);
    id<MTLBuffer> bPjW = makeBufFromFloats(dev, pjW, Npj_w);
    id<MTLBuffer> bPjB = makeBufFromFloats(dev, pjB, C);
    id<MTLBuffer> bRes = makeBufFromFloats(dev, res, Nres);
    id<MTLBuffer> bOut = [dev newBufferWithLength:Nout*2 options:MTLResourceStorageModeShared];

    uint32_t consts[] = {(uint32_t)C_pj_in, (uint32_t)C, (uint32_t)Wd, (uint32_t)H, (uint32_t)DW_K};
    id<MTLComputePipelineState> pso = makePSO(dev, lib, @"fused_ane_dw_silu_proj_res", consts, 5);
    if (!pso) return 1;

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ec = [cb computeCommandEncoder];
    [ec setComputePipelineState:pso];
    [ec setBuffer:bIn  offset:0 atIndex:0];
    [ec setBuffer:bDwW offset:0 atIndex:1];
    [ec setBuffer:bDwB offset:0 atIndex:2];
    [ec setBuffer:bPjW offset:0 atIndex:3];
    [ec setBuffer:bPjB offset:0 atIndex:4];
    [ec setBuffer:bRes offset:0 atIndex:5];
    [ec setBuffer:bOut offset:0 atIndex:6];
    [ec dispatchThreads:MTLSizeMake(Wd, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [ec endEncoding];
    [cb commit]; [cb waitUntilCompleted];

    float *got = malloc(Nout*sizeof(float));
    bufToFloats(bOut, got, Nout);

    float max_diff = 0, mean_diff = 0;
    for (int i = 0; i < Nout; ++i) {
        float d = fabsf(ref[i] - got[i]);
        max_diff = fmaxf(max_diff, d);
        mean_diff += d;
    }
    mean_diff /= Nout;
    printf("  fused_ane_dw_silu_proj_res (DW=%dx%d)  max_diff=%.5f  mean_diff=%.5f  ref[0]=%.4f got[0]=%.4f\n",
           DW_K, DW_K, max_diff, mean_diff, ref[0], got[0]);
    int ok = max_diff < 0.05f;
    free(in2c); free(dwW); free(dwB); free(pjW); free(pjB); free(res);
    free(dwout); free(ref); free(got);
    return ok ? 0 : 1;
}

static int test_silu_proj_res(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLLibrary> lib)
{
    const int H = 8, Wd = 8, C = 16;
    const int C_pj_in = 2*C;
    int Nin = H*Wd*C_pj_in;
    int Nres = H*Wd*C;
    int Npj_w = C * C_pj_in;
    int Nout = H*Wd*C;

    float *in2c = malloc(Nin*sizeof(float));
    float *pjW  = malloc(Npj_w*sizeof(float));
    float *pjB  = malloc(C*sizeof(float));
    float *res  = malloc(Nres*sizeof(float));
    for (int i = 0; i < Nin; ++i) in2c[i] = sinf(0.11f*i) * 0.5f;
    for (int i = 0; i < Npj_w; ++i) pjW[i] = cosf(0.06f*i) * 0.18f;
    for (int i = 0; i < C; ++i) pjB[i] = 0.02f * (i-C/2);
    for (int i = 0; i < Nres; ++i) res[i] = sinf(0.19f*i) * 0.3f;

    float *act = malloc(Nin*sizeof(float));
    for (int i = 0; i < Nin; ++i) act[i] = silu(in2c[i]);
    float *ref = malloc(Nout*sizeof(float));
    cpu_conv1x1(act, pjW, pjB, ref, H, Wd, C_pj_in, C);
    for (int i = 0; i < Nout; ++i) ref[i] += res[i];

    id<MTLBuffer> bIn  = makeBufFromFloats(dev, in2c, Nin);
    id<MTLBuffer> bPjW = makeBufFromFloats(dev, pjW, Npj_w);
    id<MTLBuffer> bPjB = makeBufFromFloats(dev, pjB, C);
    id<MTLBuffer> bRes = makeBufFromFloats(dev, res, Nres);
    id<MTLBuffer> bOut = [dev newBufferWithLength:Nout*2 options:MTLResourceStorageModeShared];

    uint32_t consts[] = {(uint32_t)C_pj_in, (uint32_t)C, (uint32_t)Wd, (uint32_t)H};
    id<MTLComputePipelineState> pso = makePSO(dev, lib, @"fused_ane_silu_proj_res", consts, 4);
    if (!pso) return 1;

    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> ec = [cb computeCommandEncoder];
    [ec setComputePipelineState:pso];
    [ec setBuffer:bIn  offset:0 atIndex:0];
    [ec setBuffer:bPjW offset:0 atIndex:1];
    [ec setBuffer:bPjB offset:0 atIndex:2];
    [ec setBuffer:bRes offset:0 atIndex:3];
    [ec setBuffer:bOut offset:0 atIndex:4];
    [ec dispatchThreads:MTLSizeMake(Wd, H, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [ec endEncoding];
    [cb commit]; [cb waitUntilCompleted];

    float *got = malloc(Nout*sizeof(float));
    bufToFloats(bOut, got, Nout);

    float max_diff = 0, mean_diff = 0;
    for (int i = 0; i < Nout; ++i) {
        float d = fabsf(ref[i] - got[i]);
        max_diff = fmaxf(max_diff, d);
        mean_diff += d;
    }
    mean_diff /= Nout;
    printf("  fused_ane_silu_proj_res    max_diff=%.5f  mean_diff=%.5f  ref[0]=%.4f got[0]=%.4f\n",
           max_diff, mean_diff, ref[0], got[0]);
    int ok = max_diff < 0.05f;
    free(in2c); free(pjW); free(pjB); free(res);
    free(act); free(ref); free(got);
    return ok ? 0 : 1;
}


int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];

        NSError *err = nil;
        NSURL *url = [NSURL fileURLWithPath:@"./Demosaic.metallib"];
        id<MTLLibrary> lib = [dev newLibraryWithURL:url error:&err];
        if (!lib) { fprintf(stderr, "no metallib: %s\n", [[err localizedDescription] UTF8String]); return 1; }

        printf("F_ane kernels test (device: %s)\n", [[dev name] UTF8String]);
        int fails = 0;
        fails += test_conv1x1(dev, q, lib);
        fails += test_silu_proj_res(dev, q, lib);
        fails += test_dw_silu_proj_res(dev, q, lib, 3);
        fails += test_dw_silu_proj_res(dev, q, lib, 7);
        printf("%s — %d test(s) failed\n", fails == 0 ? "PASS" : "FAIL", fails);
        return fails == 0 ? 0 : 1;
    }
}
