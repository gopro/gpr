# GPR SDK conversion tests

`gpr_conversion_tests.cpp` is an exhaustive test of the GPR SDK conversion API.
It links `gpr_sdk` directly and drives every public `gpr_convert_*` entry point
over the bundled sample files in `data/samples`, validating the produced output
(TIFF container magic, parseable metadata, dimensions, size invariants, and
lossless decode round-trip equality) — not just that a call returned.

## What is covered

For every sample (Hero5/6/7/9 and both Fusion lenses):

- `gpr_parse_metadata` (dimensions, black/white levels, white-balance gains, pixel format)
- `gpr_to_raw`, `gpr_to_dng`, `gpr_to_vc5`, `gpr_to_gpr`
- `gpr_to_rgb` at every resolution (2:1, 4:1, 8:1, 16:1) in 8- and 16-bit,
  plus JPEG encoding of the 8-bit RGB (SOI/EOI marker check)
- `dng_to_raw` (and byte-for-byte equality with `gpr_to_raw`), `dng_to_dng`, `dng_to_gpr`
- `raw_to_dng`, `raw_to_gpr`
- `vc5_to_gpr`, `vc5_to_dng`, `dng_to_vc5`

Each case runs in an isolated child process (on POSIX), so a crash in one
conversion is reported as a failure and the rest of the suite still runs.

## Known-issue (xfail) support

`run_case(..., expect_fail=true)` marks a case that exercises a currently-broken
path: a failure/crash is recorded as an expected `XFAIL` (does not fail the
suite) and an unexpected pass is reported as `XPASS` (which does fail the suite,
as a reminder to remove the marker). There are currently no xfail cases — all
conversions pass.

Note: the public `gpr_check_vc5` declaration in `gpr.h` does not match its
definition, so the test detects VC5 compression by reading the TIFF Compression
tag directly instead of calling it.

## Running

```sh
# configure + build (from the repo root)
cmake -S . -B build
cmake --build build --target gpr_tools_tests

# run via ctest (multi-config generators need -C)
ctest --test-dir build -C Debug --output-on-failure

# or run the binary directly (test output on stdout; SDK logging on stderr)
./build/source/test/Debug/gpr_tools_tests            # Xcode/MSVC layout
./build/source/test/gpr_tools_tests                  # single-config layout

# optional: point at a different sample directory
./build/source/test/Debug/gpr_tools_tests /path/to/data/samples
```

Exit code is non-zero if any non-known-issue case fails or crashes.
