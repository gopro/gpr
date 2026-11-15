# Stock decoder argument and allocator boundaries

The public `vc5_decoder_process` entry now rejects absent parameters/input,
missing allocator callbacks, aliased or simultaneous RAW/RGB output descriptors,
short/non-word-aligned/non-VC-5 input, unsupported RGB settings and gain shifts
before allocating or reading the stream. The legacy CLI now explicitly requests
the RAW-only path it writes.

`tests/decoder_bounds.c` checks these rejections with a denying allocator,
then decodes the independent 128×96 stock VC-5 fixture to RAW and 16-bit RGB.
It also fails each allocation in turn and checks the returned error, empty
caller output and zero outstanding tracked allocations: **138 RAW positions and
73 RGB positions** with this fixture. These numbers describe this input and
build, not a fixed count across codec profiles or compiler settings.

The test first reproduced three actual allocator bugs: the first failed
`CreateWavelet` allocation aborted on an assertion; a later failed inverse
scratch allocation wrote through a null pointer; and a failed component array
allocation aborted on another assertion. The decoder now propagates these
out-of-memory errors through wavelet reconstruction, releases partial arrays
and transforms, and checks the final RAW/RGB allocation before returning.
ASan/UBSan testing exercises each failure position and leak detection on Pi 5.

## Damaged stock payloads

A deterministic mutation set flips one byte at 63 four-byte offsets in the
first 256 bytes and 100 additional offsets sampled with Python seed 2 from
the independent 12,648-byte stock fixture. A separate ASan/UBSan subprocess
probe against the prior decoder produced **128 aborts in 163 cases**, with the
largest group at an assertion that followed a highpass decode error. The
mutations also exposed an assertion inside a clamp function, invalid channel
and subband indexing, a zero image pitch, and signed left shifts on damaged
inverse coefficients.

The parser now returns its existing syntax/band errors instead of asserting
on those conditions; the inverse uses defined multiplication for its zero/one
bit descale; the clamp performs its documented saturation. Channel/subband and
image-pitch checks precede indexing or allocation. Reduced RGB conversion
also checks that its required wavelets exist before reading them.

The **same 163 mutations** now produce **zero aborts and zero sanitizer
diagnostics** in the independent subprocess probe: 106 RAW decodes return an
error and 57 produce a full-size image. The native test repeats those cases
in-process with tracked allocator cleanup, then applies them to reduced RGB:
38 RGB decodes return an error and 125 produce the expected-size preview.
Decodable mutations may affect coefficients or ignored portions; the test
does not certify the resulting image as matching the source. GVID CRC checks
still reject accidental payload corruption before ordinary clip playback.

On 2026-10-29, the full Mac Release suite passed **21/21** tests and the full
Pi 5 Release suite passed **19/19**. The selected Pi ASan/UBSan/leak-detection
test passed in **5.01 s**; the same target passed local ASan/UBSan. All four
relocated package profiles passed their independent C/C++ client checks. The
legacy decoder CLI decoded the stock fixture to a 24,576-byte RAW image after
its output-mode correction; a four-byte invalid input now exits with a decode
error instead of asserting that RAW output was produced.

The resumed local pass also passes **21/21 Release targets** (14.05 s) with
the damaged-payload follow-up present. Its already-completed configured
ASan/UBSan suite passes **19/19** (59.11 s, Apple leak detection disabled).
The Pi counts above describe the preceding committed allocator-boundary
revision, not a refreshed Pi run of these mutation fixes. Further sanitizer
and malformed-stream expansion is deferred while the core still/Mission 1
workflow and local delivery are completed.

This is an allocator, public-call and selected malformed-input audit. It does
**not** prove arbitrary malformed VC-5 entropy data is safe, that all legacy
SDK error paths are leak free, or that the full SDK is thread safe. Camera
preview separately validates its strict stock payload structure before
entering this decoder.
