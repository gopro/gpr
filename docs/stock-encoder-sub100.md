# Sub-100 ms stock encoder experiment

The second phase of the architecture prototype reaches a **95.736 ms median**
on Raspberry Pi 5 for the 8192×6144 GP011689 frame with a reused allocation
workspace. This is an elapsed codec-call measurement; quality and output bytes
are unchanged.

## Measurement boundary

The 31-encode validation run includes the first encode, with no discarded
samples: 27/31 were below 100 ms, range 95.150–129.521 ms. A subsequent nine-run
sample check measured 96.412 ms median for the same frame. These are medians,
not worst-case latency guarantees.

The sample application's optional arena reserves and first-touches 768 MiB
before timing. This configuration uses 385,783,296 bytes of the arena per
frame (about 368 MiB) and resets it after all output consumers finish. Input
file I/O, workspace setup, output file I/O and the application's verification
hash are excluded from the codec-call timer. The entire codec call, including
its allocations through the arena callbacks and final bitstream assembly,
remains timed.

With ordinary per-frame malloc/free, the latest matching 31-run validation
measured **128.958 ms median**, range 120.019–155.276 ms. Earlier controlled
runs measured about 104–105 ms. Allocation/layout effects are substantial;
the reused-workspace number must not be advertised as default cold latency.

Nine-run reused-workspace medians on the complete real-input sample set:

| Input | Resolution | Median | Runs below 100 ms |
|---|---|---:|---:|
| GP010114 | 8192×6144 | 100.993 ms | 0/9 |
| GP010130 | 8192×6144 | 106.101 ms | 0/9 |
| GP010136 | 8192×6144 | 96.282 ms | 7/9 |
| GP011689 | 8192×6144 | 96.412 ms | 7/9 |
| GP011728 | 4096×3072 | 28.934 ms | 9/9 |
| GPAB1094 | 4096×3072 | 28.883 ms | 9/9 |

Thus sub-100 ms is demonstrated for two 50 MP images, with a reusable workspace.
It is not established for every image or quality setting.

## Changes that contribute

- Encode each quantized highpass row immediately into its own band bitstream.
  Keep pending zero-run and partial-word state across rows; append the band
  marker only after the final row. This removes the highpass coefficient
  write/read pass and preserves the exact stock run representation.
- Prepare signed codewords once, gather four independently, and combine
  short groups before updating the 64-bit bit reservoir. A nonzero endpoint
  bounds interior zero runs so literal-zero bits remain byte-exact. Longer
  groups and long zero runs use the scalar fallback.
- Use paired AArch64 loads and bitfield extraction in a handwritten channel
  unpack kernel, with independent LUT loads and packed NEON arithmetic.
- Deinterleave eight horizontal coefficient pairs at a time, using sliding
  vector sums/differences rather than repeated overlapping loads. Preserve
  the stock prescale rounding and scalar borders/tails.
- Optionally retain only one physical highpass row per band, assemble finished
  band payloads directly into the final stock stream, and pin one channel to
  each allowed CPU. The caller's affinity is restored after its worker returns.
- Retain the earlier 64-byte buffer/scratch alignment, cache-skewed pitches
  and correct custom allocator ownership.

The source and generated AArch64 instructions were reviewed with the
[FFmpeg assembly lessons](https://github.com/FFmpeg/asm-lessons) in mind:
register-resident values, independent work, vector arithmetic, efficient
addressing and compact loops. No assembly from that project is copied.

Shared full-frame log preprocessing, a NEON windowed `TBL` lookup, generic
eight-codeword packing, Cortex-A76-specific compilation and profile-guided
compilation were tried but did not provide a reliable additional win. The
failed source variants have been removed from the current implementation.

## Reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target vc5_encoder_app --parallel 4
GPR_STREAM_UNPACK=1 GPR_BAND_STREAM=1 GPR_CHANNEL_ASM=1 \
GPR_PIN_CHANNELS=1 GPR_DIRECT_BANDS=1 GPR_COMPACT_BANDS=1 \
GPR_BENCH_ARENA=1 GPR_BENCH_RUNS=31 \
  build/source/app/vc5_encoder_app/vc5_encoder_app \
  -i GP011689.raw -o output.vc5 -w 8192 -h 6144 -p 16384
```

Use `Encode wall`; the historical `Encoding` line measures aggregate CPU time.
Omit `GPR_BENCH_ARENA` to include normal per-frame allocation behavior.

The band-stream and compact-band policies are experimental and must be used
with the parallel memory-output encoder. Windows support and failure-path
hardening remain before upstream promotion.

## Correctness

All **108** comparisons against the parent stock encoder passed under ASan
and UBSan: six real RAW inputs, twelve synthetic/tail inputs, all six stock
quality presets, with streaming bands, direct payload assembly, compact
highpass storage and channel assembly enabled. Every sampled Pi output hash
also matched its parent reference. GP011689 remains:

`196712a7521e76ad96705dba7f3e52e3785e13330dc04ccc3bb7da6489c7e3f9`
