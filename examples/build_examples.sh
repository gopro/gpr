#!/usr/bin/env bash
#
# Builds all example programs against the pre-built static libraries in
# ../build. Run from the repo root:
#     ./examples/build_examples.sh
#
# Outputs binaries to /tmp/. Compiles with -Wall -Wextra and treats
# warnings as errors so downstream users see clean builds.

set -euo pipefail

# Locate repo root (parent of this script's directory).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="$REPO_ROOT/build"
OUT_DIR="${OUT_DIR:-/tmp}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "error: build/ directory not found. Run cmake + make in the repo root first." >&2
    exit 1
fi

CFLAGS="-O2 -Wall -Wextra -Werror"

echo ">>> Building encode_video (C, vc5_encoder only)"
clang $CFLAGS \
    -Isource/lib/vc5_encoder \
    -o "$OUT_DIR/encode_video" \
    examples/encode_video.c \
    "$BUILD_DIR/source/lib/vc5_encoder/libvc5_encoder.a" \
    "$BUILD_DIR/source/lib/vc5_common/libvc5_common.a" \
    -lpthread -lm

echo ">>> Building decode_dng (C++, full gpr_sdk + DNG SDK)"
clang++ $CFLAGS \
    -Isource/lib/gpr_sdk/public \
    -Isource/lib/common/public \
    -Isource/lib/vc5_common \
    -o "$OUT_DIR/decode_dng" \
    examples/decode_dng.cpp \
    "$BUILD_DIR/source/lib/gpr_sdk/libgpr_sdk.a" \
    "$BUILD_DIR/source/lib/vc5_encoder/libvc5_encoder.a" \
    "$BUILD_DIR/source/lib/vc5_decoder/libvc5_decoder.a" \
    "$BUILD_DIR/source/lib/vc5_common/libvc5_common.a" \
    "$BUILD_DIR/source/lib/dng_sdk/libdng_sdk.a" \
    "$BUILD_DIR/source/lib/xmp_core/libxmp_core.a" \
    "$BUILD_DIR/source/lib/expat_lib/libexpat_lib.a" \
    "$BUILD_DIR/source/lib/md5_lib/libmd5_lib.a" \
    "$BUILD_DIR/source/lib/tiny_jpeg/libtiny_jpeg.a" \
    "$BUILD_DIR/source/lib/common/libcommon.a" \
    -lpthread -lm

echo "done. Binaries in $OUT_DIR:"
ls -la "$OUT_DIR/encode_video" "$OUT_DIR/decode_dng"
