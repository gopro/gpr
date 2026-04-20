# Production Readiness Audit — 2026-04-19

## Critical Issues

### 1. Error handling: assert() used for runtime errors
- 150+ `assert()` calls become no-ops in release builds (`NDEBUG`)
- `assert(error == CODEC_ERROR_OKAY)` after recoverable calls silently swallows errors
- `assert(0)` in decoder tag/format switches falls through with undefined behavior
- **Fix**: Replace with proper error returns

### 2. Memory safety: integer overflow in ANS
- `ans.c:164` — `max_pairs = width * height + ...` overflows int32 for >16K images
- `noise_model.c:241` — `npixels = width * height` same issue
- **Fix**: Use size_t, add overflow guards

### 3. ANS input validation: untrusted data from bitstream
- `pair_count`, `rans_size`, `sign_bytes` read from file with no upper-bound check
- `sign_data = p + rans_size` can point outside buffer with crafted header
- Zero-frequency symbol in deserialized table causes division by zero
- **Fix**: Validate all sizes against `in_size`, check freq > 0

## Important Issues

### 4. API surface: internal header leaked
- `gpr.h` includes `noise_model.h` directly, exposing `fpn_model` struct internals
- **Fix**: Forward-declare, move to private header

### 5. Documentation: no docs for new features
- README only describes original GPR codec
- No usage examples for -D, -A, -R, -F flags
- No changelog or migration guide

### 6. Test coverage: no fuzz testing
- No libFuzzer target for `ans_decode_band` or `ans_deserialize_tables`
- No edge-case unit tests (1x1, all-zero, all-saturated, odd dimensions)
- `ans_test.c` not built by CMake

### 7. Thread safety: fallback path needs audit
- If pthread_create fails for channel 0 but succeeds for 1/2, fallback runs on main thread while others run concurrently
- Needs confirmation that ForwardTransformThread is read-only on shared state

## Nice-to-Have

### 8. Build system: GLOB is fragile
- `file(GLOB *.c)` doesn't trigger reconfigure when files added
