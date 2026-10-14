#!/usr/bin/env bash
# Link a public SDK consumer without build-tree headers or libraries.
set -euo pipefail
if [ "${1:-}" = --build-tree ] && [ "$#" -eq 2 ]; then
    root=$(cd "$(dirname "$0")/../.." && pwd)
    build=$(cd "$2" && pwd)
    stage=$(mktemp -d "${TMPDIR:-/tmp}/gpr-sdk-stage.XXXXXX")
    trap 'rm -rf -- "$stage"' EXIT
    mkdir -p "$stage/lib" "$stage/include"
    for lib in gpr_sdk tiny_jpeg dng_sdk xmp_core vc5_decoder vc5_encoder vc5_common common md5_lib expat_lib; do
        cp "$build/source/lib/$lib/lib$lib.a" "$stage/lib/"
    done
    while IFS= read -r -d '' header; do
        relative=${header#"$root/source/lib/"}
        mkdir -p "$(dirname "$stage/include/$relative")"
        cp "$header" "$stage/include/$relative"
    done < <(find "$root/source/lib" -type f -name '*.h' -print0)
    bash "$0" "$stage"
    exit 0
fi
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <staged-release-root> | --build-tree <build-dir>" >&2
    exit 2
fi
stage="$(cd "$1" && pwd)"
scratch="$(mktemp -d "${TMPDIR:-/tmp}/gpr-sdk-smoke.XXXXXX")"
trap 'rm -rf -- "$scratch"' EXIT
libs=()
for lib in gpr_sdk tiny_jpeg dng_sdk xmp_core vc5_decoder vc5_encoder vc5_common common md5_lib expat_lib; do
    archive="$stage/lib/lib$lib.a"
    test -f "$archive"
    libs+=("$archive")
done
case "$(uname -s)" in
    Linux) libs=(-Wl,--start-group "${libs[@]}" -Wl,--end-group) ;;
    Darwin) ;; # Apple ld rescans static archives; GNU group flags are unsupported.
    *) echo "Unsupported SDK smoke platform" >&2; exit 2 ;;
esac
# Volatile API references retain encode/decode dependencies even with dead stripping.
"${CXX:-c++}" -std=c++11 -DGPR_READING=1 -DGPR_WRITING=1 \
    -I"$stage/include/gpr_sdk/public" -I"$stage/include/common/public" \
    -I"$stage/include/vc5_common" -x c++ - -x none \
    "${libs[@]}" -pthread -lm -o "$scratch/consumer" <<'CPP'
#include <cstdlib>
#include "gpr.h"
int main() {
    auto volatile encode = &gpr_convert_raw_to_gpr;
    auto volatile decode = &gpr_convert_gpr_to_raw;
    auto volatile dng = &gpr_convert_raw_to_dng;
    gpr_parameters parameters;
    gpr_parameters_set_defaults(&parameters);
    gpr_parameters_destroy(&parameters, std::free);
    return encode && decode && dng ? 0 : 1;
}
CPP
"$scratch/consumer"
echo "PASS: installed SDK consumer"
