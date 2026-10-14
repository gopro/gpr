#!/usr/bin/env bash
# Build against an existing native build; all outputs stay under TMPDIR.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
BUILD_LIB="${BUILD_DIR:-$REPO/build}/source/lib"
OUT="${GPR_FUZZ_BUILD_DIR:-${TMPDIR:-/tmp}/gpr-fuzz}"
mkdir -p "$OUT"
INCS=("-I$REPO/source/lib/vc5_common" "-I$REPO/source/lib/vc5_encoder"
      "-I$REPO/source/lib/vc5_decoder" "-I$REPO/source/lib/common/public"
      "-I$REPO/source/lib/common/private")
LIBS=("$BUILD_LIB/vc5_decoder/libvc5_decoder.a"
      "$BUILD_LIB/vc5_encoder/libvc5_encoder.a"
      "$BUILD_LIB/vc5_common/libvc5_common.a"
      "$BUILD_LIB/common/libcommon.a" -lm -lpthread)
# Match sanitizer instrumentation used for the static libraries.
read -r -a EXTRA_CFLAGS <<< "${GPR_TEST_CFLAGS:-}"
read -r -a EXTRA_LDFLAGS <<< "${GPR_TEST_LDFLAGS:-}"
target="${1:-standalone}"
case "$target" in
  standalone)
    "${CC:-cc}" -O1 -g ${EXTRA_CFLAGS[@]+"${EXTRA_CFLAGS[@]}"} "${INCS[@]}" \
      "$HERE/main.c" "$HERE/standalone.c" "${LIBS[@]}" \
      ${EXTRA_LDFLAGS[@]+"${EXTRA_LDFLAGS[@]}"} -o "$OUT/fuzz_decoder_standalone"
    ;;
  fuzzer)
    # Use a Clang distribution with libFuzzer (upstream LLVM on macOS).
    "${FUZZ_CC:-clang}" -O1 -g -fsanitize=fuzzer,address \
      -fno-omit-frame-pointer "${INCS[@]}" "$HERE/main.c" \
      "${LIBS[@]}" -o "$OUT/fuzz_decoder"
    "${FUZZ_CC:-clang}" -O1 -g -fsanitize=fuzzer,address \
      -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION "${INCS[@]}" \
      -I"$REPO/source/app/ans_test" "$REPO/source/app/ans_test/ans.c" \
      "$REPO/source/app/fuzz_ans/fuzz_ans.c" \
      "$BUILD_LIB/vc5_common/libvc5_common.a" "$BUILD_LIB/common/libcommon.a" \
      -lm -lpthread -o "$OUT/fuzz_ans"
    ;;
  both)
    "$0" standalone
    "$0" fuzzer
    ;;
  *) echo "usage: $0 [standalone|fuzzer|both]" >&2; exit 2 ;;
esac
printf 'Fuzz binaries: %s\n' "$OUT"
