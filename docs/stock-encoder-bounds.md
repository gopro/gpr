# Encoder geometry, source capacity and legacy alignment

An instrumented boundary probe reproduced a heap-buffer overflow when the
historical encoder processed an 8x8 input. A separate 128x96 request with only
two advertised input bytes was accepted. The latter passed through an assembly
path; absence of an ASan report did not establish that its reads were valid.
The public encoder now checks geometry, row extent, format, quality and both
allocator callbacks before allocating or entering the unpack/transform code.

## Supported input contract

- Both sensor dimensions must be even and at least 48, fit the codec's 16-bit
  dimension fields, and satisfy `width * height * 2 <= INT_MAX`. Three wavelet
  levels require at least six samples at the final forward filter's input.
  Even dimensions such as 50x50 remain supported; the low-level encoder does
  not acquire the video container's separate multiple-of-eight restriction.
- `input_pitch` is a positive row stride in **bytes**, at least `width*2` for
  word inputs or `width*3/2` for packed-12. The default 4000-pixel RGGB14 image
  now has a consistent 8000-byte default stride.
- Advertised capacity must cover `(height-1)*pitch + active_row_bytes`, with
  checked arithmetic. Final-row trailing padding is optional. The caller must
  supply real readable storage for that extent; a length check does not
  validate an arbitrary pointer. Input and output buffer descriptors must be
  distinct.
- Formats and presets must be members of the supported public enums. Both
  allocation callbacks are required. Invalid geometry/extent/selection is
  rejected before codec allocation; no source bytes are modified.

Native capture creation and legacy/native calibrated preview also reject
dimensions below 48 before starting workers or decoding. The GVID framing
representation/parser remains unchanged: a syntactically representable clip
header is not proof that its image dimensions are supported by the codec.
Plain uncompressed DNG packaging retains its own geometry contract.

## Alignment without changing the ordinary fast path

Historical word/packed-12 unpackers use uint16 row addressing. An odd byte
stride would round their second-row address, and an unaligned word pointer
would violate scalar C alignment requirements. For these layouts, the public
encoder copies active rows to a 64-byte-aligned temporary only when the source
address or stride is odd. Packed-12 rows with odd active byte counts receive an
even temporary stride. The temporary uses the caller's allocator and is freed
through its matching original base pointer on success or failure.

Aligned, even-stride callers continue directly into the existing scalar,
NEON/assembly and streaming paths. No full-frame scan or copy is added there.
The newer word-Bayer fallbacks already use memcpy and continue accepting
arbitrary byte alignment/stride directly. Normalizing an incompatible legacy
layout does add a copy and temporary storage; no zero-copy claim applies to
that case. The internal image descriptor now records the checked input span
without signed pixel-count multiplication.

This follows the alignment discipline discussed in the user's
[FFmpeg assembly guide, lesson three](https://github.com/FFmpeg/asm-lessons/blob/171e7261abcdd69e702c4c9b4bbb6f67fe0ade83/lesson_03/index.md):
make alignment assumptions explicit before using a specialized kernel. Here
the check also protects the historical C row-address calculation on ARM.

## Validation

`encoder_bounds_test` covers invalid/overflowing geometry, negative/short stride,
short capacity, invalid enums, missing callbacks and descriptor aliasing before
any codec allocation. A deliberately failing allocator verifies both the
ordinary first allocation and aligned-scratch failure. The test preserves
source canaries and compares **756** unaligned/padded/odd-stride results against
their aligned equivalents across all 14 encoder input selections, six presets
and 48x48, 50x50 and 72x96 geometry. It runs with ordinary flags and again with
streaming/band/assembly flags, for **1,512** comparisons in total. Exact final
active-row capacity is used, without relying on trailing allocation padding.

Capture/preview tests reject every multiple-of-eight dimension below 48.
Existing Bayer conformance still compares the independent historical stock
fixtures and exercises all formats/presets with ordinary and fast flags. A
separate ASan/UBSan probe also encodes/decodes 48x48, 50x50, 56x56 and 72x72.
The original tiny/truncated probes are rerun after the repair. Revision-specific
Pi correctness, sanitizer, before/after 12 MP timing and package receipts are
in `stock-encoder-bounds-receipts/`.

On Pi 5, three interleaved pairs of 24-frame 4096x3072 Q3 encodes measured
**25.008 ms before / 25.046 ms after** pooled median encoding time (72 frames
per binary). All 144 compressed frames match exactly before/after and retain
the earlier independent stock payload identities. Encoder workspace peak is
unchanged at 97,936,896 bytes. The 0.15% median difference is within the spread
between these short runs; this is no evidence of a material throughput change.
The fixture replay includes no sensor, preview or CNN. Final fsync and complete
wall rates are reported separately and are not included in encode-only timing.

This is the encoder/capture/preview boundary increment. It does not complete
arbitrary compressed-bitstream decoder fuzzing, every SDK failure path or full
still-SDK concurrency validation. Those remain in the feature rebuild audit.
