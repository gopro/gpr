/*! @file main.c
 *  @brief libFuzzer entry point for the GPR raw-video decoder.
 *
 *  Container, entropy, and bounded image decode are exercised:
 *
 *    1. gpr_video_read_clip_header / gpr_video_read_frame_header
 *       (the container parser — first thing a decoder sees on the wire).
 *
 *    2. jans_decode_band_x4 (the rANS band decoder used by the fused
 *       pipeline) on each frame's payload slice, for a few representative
 *       band dimensions.
 *    3. gpr_decode_fused on payloads with dimensions at most 512 x 512.
 *
 *  The harness:
 *    - never aborts/crashes/OOMs on malformed input (that is the bug we'd
 *      file against the lib);
 *    - clamps parsed dims to a sane max (10K x 10K) before allocating;
 *    - skips bands whose declared payload exceeds remaining input.
 *
 *  Build: see build.sh.
 *
 *  Licensed under Apache-2.0 or MIT.
 */

#include "gpr_video_format.h"
#include "ans_joint.h"
#include "fused_encode.h"
#include "fused_decode.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Sanity bounds: cap allocations so a maliciously huge width/height in
   the parsed header cannot OOM the fuzzer. 10K x 10K = 100M coeffs ~ 400 MB
   as int32 — too large for a per-call alloc; we cap output band area instead. */
#define MAX_BAND_WIDTH   2048
#define MAX_BAND_HEIGHT  2048
#define MAX_INPUT_SIZE   (16 * 1024 * 1024)   /* skip absurdly large inputs */

/* Representative band shapes for jans_decode_band_x4 — production uses a
   range of widths/heights per wavelet level. We pick three to cover the
   loop body once, twice, and many times. */
static const struct { int w, h; } kBandShapes[] = {
    {  16,   16 },
    { 128,   96 },
    { 512,  384 },
};
#define NUM_BAND_SHAPES ((int)(sizeof(kBandShapes) / sizeof(kBandShapes[0])))

static void try_decode_band(const uint8_t *payload, size_t payload_size, int shape_idx)
{
    if (shape_idx < 0 || shape_idx >= NUM_BAND_SHAPES) return;
    int w = kBandShapes[shape_idx].w;
    int h = kBandShapes[shape_idx].h;
    if (w <= 0 || h <= 0 || w > MAX_BAND_WIDTH || h > MAX_BAND_HEIGHT) return;

    int32_t *band = (int32_t *)calloc((size_t)w * (size_t)h, sizeof(int32_t));
    if (!band) return;
    int pitch = w * (int)sizeof(int32_t);

    /* jans_decode_band_x4 must tolerate arbitrary bytes. We don't check
       the return value — we only care that it doesn't crash. */
    (void)jans_decode_band_x4(payload, payload_size, band, w, h, pitch);

    free(band);
}

static void try_decode_frame(const uint8_t *payload, size_t size)
{
    if (size < sizeof(FUSED_HEADER)) return;
    FUSED_HEADER header;
    memcpy(&header, payload, sizeof(header));
    /* Bound image dimensions before the decoder allocates wavelet buffers. */
    if (header.magic != FUSED_MAGIC || !header.width || !header.height ||
        header.width > 512 || header.height > 512) return;
    uint16_t *bayer = calloc((size_t)header.width * header.height, sizeof(*bayer));
    if (!bayer) return;
    int width = 0, height = 0;
    (void)gpr_decode_fused(payload, size, bayer, header.width * sizeof(*bayer),
                           &width, &height);
    free(bayer);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > MAX_INPUT_SIZE) return 0;

    /* ---- 1. Container parse ---- */
    gpr_video_clip_header clip;
    memset(&clip, 0, sizeof(clip));
    int parsed_clip = (gpr_video_read_clip_header(data, size, &clip) == 0);

    size_t off = 0;
    if (parsed_clip) {
        off = GPR_VIDEO_CLIP_HEADER_SIZE;
        /* Pre-touch dims (clamped). If clip dims are insane the loop below
           still bounds work by payload size, not by clip dims. */
        (void)clip.width;
        (void)clip.height;
    }

    /* ---- 2. Frame-loop: parse each frame header and fuzz its payload ---- */
    int frame_idx = 0;
    const int MAX_FRAMES = 64;   /* don't spin forever on adversarial input */
    while (frame_idx < MAX_FRAMES && off + GPR_VIDEO_FRAME_HEADER_SIZE <= size) {
        gpr_video_frame_header fh;
        memset(&fh, 0, sizeof(fh));
        if (gpr_video_read_frame_header(data + off, size - off, &fh) != 0) {
            /* Magic mismatch / short read — advance one byte to keep
               looking for plausible payloads in this input. */
            off += 1;
            frame_idx++;
            continue;
        }
        off += GPR_VIDEO_FRAME_HEADER_SIZE;

        /* Skip frames whose declared payload exceeds what's left. */
        if (fh.payload_size == 0 || fh.payload_size > size - off) {
            frame_idx++;
            /* Don't try to skip ahead by the bogus declared size — just
               break, since downstream offsets would be garbage anyway. */
            break;
        }

        const uint8_t *payload = data + off;
        size_t psize = fh.payload_size;

        try_decode_frame(payload, psize);

        /* Try a few band shapes against this payload. */
        for (int s = 0; s < NUM_BAND_SHAPES; s++) {
            try_decode_band(payload, psize, s);
        }

        off += psize;
        frame_idx++;
    }

    /* ---- 3. Also fire jans_decode_band_x4 once on the raw input, in
            case there's no valid container. Catches band-decoder bugs
            without needing a parseable header. ---- */
    if (size <= 1 * 1024 * 1024) {
        try_decode_band(data, size, 0 + ((int)(data[0] % NUM_BAND_SHAPES)));
        try_decode_frame(data, size);
    }

    return 0;
}
