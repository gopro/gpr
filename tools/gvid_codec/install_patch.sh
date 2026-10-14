#!/usr/bin/env bash
# install_patch.sh — apply the GPR decoder patch to a vanilla FFmpeg tree.
#
# Usage:
#   ./install_patch.sh /path/to/FFmpeg_source
#
# After this completes, run configure_and_build.sh in the FFmpeg tree.

set -euo pipefail

GPR_EXTERNAL_ROOT="${GPR_EXTERNAL_ROOT:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/gpr_work}"
FFROOT="${1:-${FF_ROOT:-$GPR_EXTERNAL_ROOT/external/ffmpeg_gpr}}"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

[ -d "$FFROOT/libavcodec" ] || { echo "not an FFmpeg tree: $FFROOT" >&2; exit 1; }
[ "$(git -C "$FFROOT" describe --tags --exact-match HEAD 2>/dev/null)" = n8.0 ] || {
    echo "patch requires FFmpeg tag n8.0" >&2; exit 1;
}

cp "$SELF_DIR/gpr.c" "$FFROOT/libavcodec/gpr.c"
echo "installed: libavcodec/gpr.c"

apply_once() {
    local file="$1" anchor="$2" insertion="$3"
    if grep -qF "$insertion" "$file"; then
        echo "skip (already present): $file"
        return 0
    fi
    if ! grep -qF "$anchor" "$file"; then
        echo "ERROR: anchor not found in $file: $anchor" >&2
        exit 1
    fi
    # GNU vs BSD awk both handle the same script.
    awk -v a="$anchor" -v ins="$insertion" '
        { print }
        index($0, a) { print ins }
    ' "$file" > "$file.new" && mv "$file.new" "$file"
    echo "patched: $file"
}

# libavcodec/Makefile — add OBJS-$(CONFIG_GPR_DECODER)
apply_once "$FFROOT/libavcodec/Makefile" \
    "OBJS-\$(CONFIG_CFHD_ENCODER)            += cfhdenc.o cfhddata.o cfhdencdsp.o" \
    "OBJS-\$(CONFIG_GPR_DECODER)             += gpr.o"

# libavcodec/allcodecs.c — add extern decl
apply_once "$FFROOT/libavcodec/allcodecs.c" \
    "extern const FFCodec ff_cfhd_decoder;" \
    "extern const FFCodec ff_gpr_decoder;"

# libavcodec/codec_id.h — append AV_CODEC_ID_GPR just before the
# AV_CODEC_ID_FIRST_AUDIO sentinel. New codec IDs must go at the END
# of the video block — libavcodec/version.c has a static_assert that
# pins existing ID values to keep ABI stable.
apply_once "$FFROOT/libavcodec/codec_id.h" \
    "    AV_CODEC_ID_PRORES_RAW," \
    "    AV_CODEC_ID_GPR,"

# libavcodec/codec_desc.c — insert descriptor at the end (after
# PRORES_RAW). Order in codec_descriptors[] MUST match the enum order
# in codec_id.h — the array is bsearch'd by id.
if ! grep -q "AV_CODEC_ID_GPR" "$FFROOT/libavcodec/codec_desc.c"; then
    python3 - "$FFROOT/libavcodec/codec_desc.c" <<'PY'
import sys, re, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
needle = re.compile(
    r"(\{\s*\.id\s*=\s*AV_CODEC_ID_PRORES_RAW,.*?\},)\s*\n(\s*/\* various PCM)",
    re.DOTALL,
)
m = needle.search(src)
if not m:
    sys.exit("codec_desc.c: PRORES_RAW descriptor not found")
ins = """
    {
        .id        = AV_CODEC_ID_GPR,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "gpr",
        .long_name = NULL_IF_CONFIG_SMALL("GoPro RAW (fused VC-5)"),
        .props     = AV_CODEC_PROP_INTRA_ONLY | AV_CODEC_PROP_LOSSY,
    },"""
src = src[:m.end(1)] + ins + "\n\n" + m.group(2) + src[m.end():]
p.write_text(src)
print("patched: libavcodec/codec_desc.c")
PY
else
    echo "skip (already present): libavcodec/codec_desc.c"
fi

# libavformat/isom_tags.c — add codec_tag mapping after CFHD line.
python3 - "$FFROOT/libavformat/isom_tags.c" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text()
anchor = "    { AV_CODEC_ID_CFHD, MKTAG('C', 'F', 'H', 'D') },\n"
if anchor not in src:
    sys.exit("isom_tags.c: CFHD line not found")
ins = "".join(line for line in (
    "    { AV_CODEC_ID_GPR,  MKTAG('G', 'P', 'R', 'r') },\n",
    "    { AV_CODEC_ID_GPR,  MKTAG('G', 'P', 'R', '1') },\n",
) if line.strip() not in src)
src = src.replace(anchor, anchor + ins, 1)
p.write_text(src)
print("patched: libavformat/isom_tags.c")
PY

# configure registers a "GPR_DECODER" component automatically once it
# sees the extern in allcodecs.c, so no changes there are required —
# but we DO want it enabled by default. The find_things_extern scanner
# adds it to $CODEC_LIST and the default disable-all+enable-* logic
# enables it by default unless the user passed --disable-decoder=gpr.
echo
echo "Patch installation complete. Next steps:"
echo "  cd $FFROOT"
echo "  $SELF_DIR/configure_and_build.sh"
