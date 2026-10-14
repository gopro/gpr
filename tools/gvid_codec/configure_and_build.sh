#!/usr/bin/env bash
# configure_and_build.sh — build the patched FFmpeg with GPR decoder enabled.
#
# Run from the FFmpeg source root (after install_patch.sh).
#
# We deliberately go minimal to keep the build fast:
#   - no external codec deps (--disable-everything + targeted enables)
#   - just ffmpeg + ffplay + libavformat + libavcodec
#   - link our static GPR decoder libs via --extra-libs

set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GPR_ROOT="${GPR_ROOT:-$(cd "$SELF_DIR/../.." && pwd)}"
GPR_BUILD="${GPR_BUILD:-$GPR_ROOT/build}"
PREFIX="${PREFIX:-$(pwd)/install-gpr}"

[ "$(git describe --tags --exact-match HEAD 2>/dev/null)" = n8.0 ] || {
    echo "FFmpeg source must be checked out at tag n8.0" >&2; exit 1;
}
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
case "$JOBS" in ''|*[!0-9]*|0) echo "JOBS must be a positive integer" >&2; exit 1;; esac
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi

GPR_INC="$GPR_ROOT/source/lib/vc5_decoder"
GPR_LIB_D="$GPR_BUILD/source/lib/vc5_decoder"
GPR_LIB_C="$GPR_BUILD/source/lib/vc5_common"
GPR_LIB_X="$GPR_BUILD/source/lib/common"

for lib in "$GPR_LIB_D/libvc5_decoder.a" \
           "$GPR_LIB_C/libvc5_common.a" \
           "$GPR_LIB_X/libcommon.a"; do
    [ -f "$lib" ] || { echo "missing static lib: $lib (build the GPR repo first)" >&2; exit 1; }
done

# SDL2 is required for ffplay; brew install sdl2 first if missing.
SDL_FLAGS=(--disable-sdl2 --disable-ffplay)
if pkg-config --exists sdl2 2>/dev/null; then
    SDL_FLAGS=(--enable-sdl2 --enable-ffplay)
fi

./configure \
    --prefix="$PREFIX" \
    --disable-doc \
    --disable-htmlpages \
    --disable-manpages \
    --disable-podpages \
    --disable-txtpages \
    --disable-shared \
    --enable-static \
    --disable-network \
    --disable-everything \
    --disable-autodetect \
    --enable-zlib \
    --enable-demuxer=mov \
    --enable-muxer=mov \
    --enable-protocol=file \
    --enable-decoder=gpr \
    --enable-decoder=rawvideo \
    --enable-encoder=rawvideo \
    --enable-encoder=wrapped_avframe \
    --enable-encoder=ffv1 \
    --enable-encoder=prores \
    --enable-encoder=prores_ks \
    --enable-decoder=prores \
    --enable-muxer=rawvideo \
    --enable-muxer=null \
    --enable-muxer=image2 \
    --enable-muxer=md5 \
    --enable-encoder=png \
    --enable-encoder=tiff \
    --enable-decoder=png \
    --enable-decoder=tiff \
    --enable-filter=scale \
    --enable-filter=format \
    --enable-filter=null \
    --enable-filter=buffer \
    --enable-filter=buffersink \
    --enable-filter=transpose \
    --enable-filter=hflip \
    --enable-filter=vflip \
    --enable-ffmpeg \
    --enable-ffprobe \
    "${SDL_FLAGS[@]}" \
    --extra-cflags="-I$GPR_INC" \
    --extra-ldflags="-L$GPR_LIB_D -L$GPR_LIB_C -L$GPR_LIB_X" \
    --extra-libs="-lvc5_decoder -lvc5_common -lcommon -lm -lpthread"

grep -q '^#define CONFIG_GPR_DECODER 1' config_components.h || {
    echo "GPR decoder was not configured" >&2; exit 1;
}

echo
echo "Configure OK. Building ffmpeg (+ffplay if SDL2 was found)..."
make -j"$JOBS"

echo
echo "Build complete. Binaries:"
ls -1 ./ffmpeg ./ffplay 2>/dev/null || true
echo
echo "Smoke test:"
./ffmpeg -hide_banner -decoders > gpr-decoders.txt 2>&1
grep -E '[[:space:]]gpr[[:space:]]' gpr-decoders.txt || {
    echo "GPR decoder not registered" >&2; exit 1;
}
