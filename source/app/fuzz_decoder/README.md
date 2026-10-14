# Decoder Fuzzing

The opt-in harness exercises GVID clip/frame parsing, the ANS band decoder,
and full `gpr_decode_fused` decoding for dimensions up to 512 x 512. Larger
frames still exercise bounded parser/ANS paths. Input and allocation limits
make fuzzing practical; they are not product dimension limits.

## Build

Build the codec libraries first from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DGPR_ENABLE_VIDEO=ON -DGPR_BUILD_DIAGNOSTICS=ON
cmake --build build --parallel 4
BUILD_DIR=build bash source/app/fuzz_decoder/build.sh
```

No argument selects standalone. `BUILD_DIR` selects the codec build;
`GPR_FUZZ_BUILD_DIR` selects harness outputs, defaulting to `gpr-fuzz` under
`TMPDIR` (or the script's temporary-directory fallback). `CC` selects the
standalone compiler. Select libFuzzer explicitly:

```bash
BUILD_DIR=build FUZZ_CC=clang bash source/app/fuzz_decoder/build.sh fuzzer
```

This builds both decoder and ANS fuzz targets. `FUZZ_CC` must provide the
Clang libFuzzer and AddressSanitizer runtimes. Apple/Xcode Clang alone does
not supply the required libFuzzer runtime; use an LLVM distribution that
does, such as Homebrew LLVM, and select its compiler through `FUZZ_CC`.
Sanitizer coverage also depends on instrumenting the linked codec build.

## Corpus and Execution

Use the retained test driver for the current generated corpus and executable
names:

```bash
BUILD_DIR=build bash tools/test/test_fuzz_corpus.sh
```

The build above opts into diagnostics because the driver generates a valid
current GVID via `bench_fused` and exercises
standalone corpus coverage. `corpus/legacy_mismatched.gvid` is the historical
malformed input formerly named `happy.gvid`; it is not a valid happy-path
fixture. Truncated and magic-corrupted inputs test rejection paths.
Sanitizer CI runs the standalone corpus and 30 seconds of libFuzzer with
ASan; that bounded run is not exhaustive decoder safety proof.

When invoking libFuzzer directly, pass a writable corpus directory. Crash
artifacts default to the working directory, not an automatically created
`crashes/` directory. To use a dedicated location, create it first and pass
`-artifact_prefix=artifacts/fuzz-crashes/`. Replay a crash by passing the
artifact filename to the same fuzz executable.

Standalone drivers report per-input results. Inspect skips and input counts:
a zero exit without readable inputs is not corpus coverage. Keep fixed crash
reproducers as regression seeds, distinguishing malformed seeds from valid
current encoded streams.
