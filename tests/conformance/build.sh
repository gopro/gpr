#!/bin/sh
# tests/conformance/build.sh — compile the conformance generate + check binaries.
#
# Produces four binaries under $TMPDIR/conformance:
#   conformance_generate_L1   conformance_generate_L2
#   conformance_check_L1      conformance_check_L2
#
# The wavelet level is a compile-time switch (-DFUSED_WAVELET_LEVELS=N) so each
# level needs its own binary. The fused_encode.c sources are pulled in directly
# (rather than via the prebuilt libvc5_encoder.a) so the macro takes effect;
# everything else still resolves from the prebuilt static libs.
#
# Prereq: top-level CMake build is already populated at ./build/ — the prebuilt
# vc5_common, md5_lib, and (for the asm helpers) vc5_encoder static libs are
# linked.

set -e

# Locate repo root (this script lives at tests/conformance/build.sh).
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR=${BUILD_DIR:-build}
GPR_TMPDIR="${GPR_TMPDIR:-${TMPDIR:-${RUNNER_TEMP:-/tmp}}}"
TMPDIR="$GPR_TMPDIR"
OUT_DIR="${CONFORMANCE_BIN_DIR:-$TMPDIR/conformance}"
mkdir -p "$OUT_DIR"

VC5_ENC_LIB=$BUILD_DIR/source/lib/vc5_encoder/libvc5_encoder.a
VC5_COMMON_LIB=$BUILD_DIR/source/lib/vc5_common/libvc5_common.a
MD5_LIB=$BUILD_DIR/source/lib/md5_lib/libmd5_lib.a

for f in "$VC5_ENC_LIB" "$VC5_COMMON_LIB" "$MD5_LIB"; do
  if [ ! -f "$f" ]; then
    echo "missing prebuilt library: $f"
    echo "build the top-level cmake project first  (cmake -S . -B $BUILD_DIR && cmake --build $BUILD_DIR)"
    exit 1
  fi
done

INCLUDES="-Isource/lib/vc5_encoder -Isource/lib/vc5_common -Isource/lib/md5_lib \
          -Isource/lib/common/public -Isource/lib/common/private"
LIBS="$VC5_ENC_LIB $VC5_COMMON_LIB $MD5_LIB -lpthread -lm"
CFLAGS="-O2 -Wall -Wno-unused-function $INCLUDES"

# Sources that need to be recompiled with the FUSED_WAVELET_LEVELS macro.
# Pulling in fused_encode.c directly causes its symbols to override the ones
# in libvc5_encoder.a (the .o files come before the archive on the link line).
SRC_FUSED=source/lib/vc5_encoder/fused_encode.c

build_one() {
  out_name="$1"; src="$2"; levels="$3"
  cmd="clang $CFLAGS -DFUSED_WAVELET_LEVELS=$levels \
       -o \"$OUT_DIR/${out_name}_L${levels}\" \
       $src $SRC_FUSED $LIBS"
  echo "  $out_name L=$levels"
  eval $cmd
}

echo "Compiling conformance binaries"
for levels in 1 2; do
  build_one conformance_generate tests/conformance/generate.c "$levels"
  build_one conformance_check    tests/conformance/check.c    "$levels"
done

echo "done."
echo "  $OUT_DIR/conformance_generate_L1   $OUT_DIR/conformance_generate_L2"
echo "  $OUT_DIR/conformance_check_L1      $OUT_DIR/conformance_check_L2"
