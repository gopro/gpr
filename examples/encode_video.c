/* encode_video.c
 *
 * Minimal example: encode a sequence of raw Bayer frames as a GPR video clip.
 *
 * Usage:
 *     encode_video <raw_input> <output_clip>
 *
 * The input is expected to be a single 8280x5520 RGGB16 Bayer frame
 * (pixel_format=4, 91,411,200 bytes). We submit it N times to simulate
 * a short clip; a real consumer would iterate over distinct buffers.
 *
 * Demonstrates:
 *   - gpr_video_encoder lifecycle (create / submit / flush / destroy)
 *   - Adaptive bitrate via gpr_video_encoder_set_target_bitrate()
 *   - Writer callback writing clip + frame headers from gpr_video_format.h
 *   - The abort path: writer returns <0 on a short write, the encoder
 *     shuts down cleanly, destroy() does not block on the flush.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpr_video.h"
#include "gpr_video_format.h"

#define WIDTH        8280
#define HEIGHT       5520
#define PIXEL_FORMAT 4         /* RGGB16 */
#define QUALITY      3         /* Filmscan-1 default */
#define RING_DEPTH   3
#define FPS          24.0
#define TARGET_MBPS  150.0
#define NUM_FRAMES   8

/* Writer context — owns the output FILE* plus the clip-header state we
   need to emit on the first frame. */
typedef struct {
    FILE *fp;
    int   header_written;
    int   width, height, pixel_format, quality;
    double fps, target_MBps;
} writer_ctx;

static int writer_fn(void *user_data,
                     const uint8_t *vc5_bitstream,
                     size_t size,
                     uint64_t frame_tag)
{
    writer_ctx *w = (writer_ctx *)user_data;

    /* First call: emit clip header. */
    if (!w->header_written) {
        uint8_t hdr[GPR_VIDEO_CLIP_HEADER_SIZE];
        if (gpr_video_write_clip_header(hdr, sizeof(hdr),
                                         w->width, w->height,
                                         w->pixel_format, w->quality,
                                         w->fps, w->target_MBps,
                                         0 /* denoise off */,
                                         NUM_FRAMES) < 0) {
            fprintf(stderr, "writer: failed to build clip header\n");
            return -1;  /* fatal */
        }
        if (fwrite(hdr, 1, sizeof(hdr), w->fp) != sizeof(hdr)) {
            fprintf(stderr, "writer: short write on clip header\n");
            return -1;
        }
        w->header_written = 1;
    }

    /* Per-frame header. */
    uint8_t fhdr[GPR_VIDEO_FRAME_HEADER_SIZE];
    if (gpr_video_write_frame_header(fhdr, sizeof(fhdr), size, frame_tag) < 0) {
        fprintf(stderr, "writer: failed to build frame header\n");
        return -1;
    }
    if (fwrite(fhdr, 1, sizeof(fhdr), w->fp) != sizeof(fhdr)) {
        fprintf(stderr, "writer: short write on frame header\n");
        return -1;
    }

    /* Bitstream payload. A short write here triggers the abort path:
       returning <0 marks the encoder aborted, drops queued frames, and
       lets destroy() tear down without waiting. */
    if (fwrite(vc5_bitstream, 1, size, w->fp) != size) {
        fprintf(stderr, "writer: short write on payload (frame %llu)\n",
                (unsigned long long)frame_tag);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <raw_input> <output_clip>\n", argv[0]);
        return 1;
    }
    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    /* Read the single raw frame into memory. */
    FILE *in = fopen(in_path, "rb");
    if (!in) { perror("fopen input"); return 1; }
    fseek(in, 0, SEEK_END);
    long raw_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    size_t expected = (size_t)WIDTH * HEIGHT * 2;
    if ((size_t)raw_size != expected) {
        fprintf(stderr, "input size %ld != expected %zu (%dx%d RGGB16)\n",
                raw_size, expected, WIDTH, HEIGHT);
        fclose(in);
        return 1;
    }
    uint8_t *raw = (uint8_t *)malloc(expected);
    if (!raw || fread(raw, 1, expected, in) != expected) {
        fprintf(stderr, "failed to read input\n");
        free(raw); fclose(in); return 1;
    }
    fclose(in);

    /* Open output and build writer context. */
    writer_ctx w;
    memset(&w, 0, sizeof(w));
    w.fp = fopen(out_path, "wb");
    if (!w.fp) { perror("fopen output"); free(raw); return 1; }
    w.width = WIDTH; w.height = HEIGHT;
    w.pixel_format = PIXEL_FORMAT; w.quality = QUALITY;
    w.fps = FPS; w.target_MBps = TARGET_MBPS;

    /* Create encoder + enable rate control. */
    GPR_VIDEO_ENCODER *enc = gpr_video_encoder_create(
        WIDTH, HEIGHT, PIXEL_FORMAT, QUALITY, RING_DEPTH,
        writer_fn, &w);
    if (!enc) {
        fprintf(stderr, "gpr_video_encoder_create failed\n");
        fclose(w.fp); free(raw); return 1;
    }
    gpr_video_encoder_set_target_bitrate(enc, TARGET_MBPS, FPS);

    /* Submit N frames, looping the same buffer. */
    for (uint64_t i = 0; i < NUM_FRAMES; i++) {
        if (gpr_video_encoder_submit(enc, raw, expected, i) != 0) {
            fprintf(stderr, "submit failed at frame %llu (encoder aborted?)\n",
                    (unsigned long long)i);
            break;
        }
    }

    /* Wait for the pipeline to drain, then tear down. If the writer
       returned <0 anywhere above, flush() returns immediately and
       destroy() skips the implicit flush. */
    gpr_video_encoder_flush(enc);

    gpr_video_stats st;
    gpr_video_encoder_get_stats(enc, &st);

    gpr_video_encoder_destroy(enc);
    fclose(w.fp);
    free(raw);

    printf("submitted=%llu  encoded=%llu  written=%llu  writer_errors=%llu\n",
           (unsigned long long)st.frames_submitted,
           (unsigned long long)st.frames_encoded,
           (unsigned long long)st.frames_written,
           (unsigned long long)st.writer_errors);
    printf("waits  submit=%llu  encoder=%llu  writer=%llu\n",
           (unsigned long long)st.submit_waited,
           (unsigned long long)st.encoder_waited,
           (unsigned long long)st.writer_waited);
    printf("wrote %s\n", out_path);
    return (st.frames_written == NUM_FRAMES) ? 0 : 2;
}
