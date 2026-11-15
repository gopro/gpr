# Stock VC-5 latency experiment

The follow-up [sub-100 ms experiment](stock-encoder-sub100.md) adds streaming
band entropy coding and a reusable workspace, reaching a 95.7 ms median on
the reference 50 MP frame. The measurements below describe the first phase.

This branch builds on GoPro PR #73 and preserves its encoded representation.
It is a prototype for Raspberry Pi 5 / AArch64, with an opt-in streaming path:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vc5_encoder_app --parallel 4
GPR_STREAM_UNPACK=1 GPR_BENCH_RUNS=21 \
  build/source/app/vc5_encoder_app/vc5_encoder_app \
  -i GP011689.raw -o output.vc5 -w 8192 -h 6144 -p 16384
```

Read `Encode wall`, which measures `CLOCK_MONOTONIC` around
`vc5_encoder_process()`. The historical `Encoding ... secs per frame` measures
`clock()`: aggregate CPU time across all worker threads. It is not capture
latency. Earlier conclusions that the threaded variants all regressed were
based on that unsuitable metric and should be disregarded.

## Architecture

- Each channel worker unpacks only the Bayer row its horizontal transform
  is about to consume. This removes four full component planes (about 100 MB
  at 50 MP) and the frame-wide unpack barrier. Supported initially for RGGB14
  widths divisible by eight; other input layouts retain the existing path.
- Workers own horizontal scratch, codec state, and channel output. Transform
  and VLC work stay together, and encoded channels are appended in their
  original order. The parent allocates and frees all worker buffers.
- Log lookup results are gathered independently, packed into 64-bit integer
  registers and transferred directly to NEON registers. The streaming loop's
  generated AArch64 code uses `fmov dN,xN` and packed `shadd` arithmetic rather
  than a stack array of individual log results. Log values and rounding are
  exactly the stock ones.
- Vector quantization uses widening 16-bit multiply where the multiplier
  fits and bypasses the identity quantizer. The generic multiply remains for
  other multipliers.
- VLC uses a 64-bit reservoir and can combine a short zero prefix with the
  following magnitude/sign codeword.

The materialized unpack fallback also has a standalone AArch64 assembly row
kernel. The streaming path currently uses intrinsics plus explicit integer
packing; handwritten assembly is not required for the measured streaming win.

## Alignment and cache placement

The later fused encoder's 64-byte alignment principle is applied to component
planes, wavelet bands, horizontal scratch and the streaming unpack row. An
alignment adapter retains the caller's original allocation pointer and passes
that pointer back to its `Free` callback. It checks allocation-size overflow.

Aligned starts alone are insufficient for power-of-two image widths. Wavelet
and component row pitches include an extra cache line; component-plane starts
are staggered by a cache line, and horizontal low/high scratch is separated
by a line. These preserve vector alignment while reducing repeated cache-set
mapping. Physical padding is excluded from VLC runs, so it does not create
extra coefficients in the stream. Thumbnail input uses the physical pitch.

The implementation follows the register/vector, compact loop, addressing,
alignment and packed-arithmetic principles reviewed in all three published
[FFmpeg assembly lessons](https://github.com/FFmpeg/asm-lessons). The lessons
use x86; instruction choices here are AArch64 equivalents, verified in the
Pi executable's disassembly.

## Raspberry Pi 5 measurements, 2026-10-26

Pi 5 Cortex-A76, 2.4 GHz performance governor. Release, NEON and IPO enabled.
The timed codec call includes normal allocation, transforms, VLC, channel
assembly and release; it excludes reading the input file and writing/hash
checking the output file. No preallocated arena is used in these results.

Twenty-one consecutive GP011689 encodes: median **174.4 ms**, range
**166.0–187.2 ms**, all identical to stock. The first shorter prototype run
had a 169.5 ms median; use the longer/fresh-process comparisons below for
conclusions rather than selecting the fastest run.

Seven alternating fresh-process measurements per version and input:

| Input | Dimensions | PR #73 median | Prototype median | Speedup |
|---|---|---:|---:|---:|
| GP010114 | 8192×6144 | 456.5 ms | 222.7 ms | 2.05× |
| GP010130 | 8192×6144 | 479.3 ms | 206.0 ms | 2.33× |
| GP010136 | 8192×6144 | 427.0 ms | 187.7 ms | 2.27× |
| GP011689 | 8192×6144 | 420.9 ms | 186.7 ms | 2.25× |
| GP011728 | 4096×3072 | 144.0 ms | 50.4 ms | 2.86× |
| GPAB1094 | 4096×3072 | 141.2 ms | 48.5 ms | 2.91× |

This crosses 200 ms for two tested 50 MP images. It does **not** establish a
sub-200 ms bound for every image or quality setting. Some individual fresh
process measurements also exceeded their median substantially.

## Verification and remaining work

`tests/stock_encoder_architecture.py` compares all six quality presets on six
real RAWs and twelve synthetic inputs (zero, maximum, alternating, random;
72/80/128-pixel widths). All **108** outputs were byte-identical under ASan and
UBSan. The synthetic widths exercise vector tails. The allocation adapter
also passes an ASan/UBSan test across 64 underlying pointer offsets, sizes
1–255, null frees and size overflow. Six bundled GPR samples retain exactly
the same decoded RAW output after the shared buffer-layout changes.

The fresh-process Pi benchmark compares output SHA-256 on every invocation.
GP011689's SHA-256 remains
`196712a7521e76ad96705dba7f3e52e3785e13330dc04ccc3bb7da6489c7e3f9`.

Before upstreaming: remove experiment-only profiling/environment controls,
finish Windows timer/compiler support, reduce temporary output capacity,
exercise allocation/thread-creation failures, and make the streaming path
an explicit supported encoder policy. The common allocation/pitch changes
also warrant wider SDK/thumbnail testing before replacing the small PR #73.
