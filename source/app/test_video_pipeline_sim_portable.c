/*! @file test_video_pipeline_sim_portable.c
 *
 *  @brief Storage-bus simulator for the pipelined video encoder.
 *
 *  Submits frames at a target framerate while a throttled writer callback
 *  simulates a storage device (microSD / CFexpress / NVMe). Bandwidth and
 *  periodic GC-stall behavior are configurable, so we can see how the
 *  pipeline architecture holds up under realistic storage constraints
 *  without needing real hardware.
 *
 *  Build:
 *    cc -O2 -o "$TMPDIR/test_video_pipeline_sim" source/app/test_video_pipeline_sim_portable.c \
 *      build/source/lib/vc5_encoder/libvc5_encoder.a \
 *      build/source/lib/vc5_common/libvc5_common.a -lpthread
 *
 *  Usage:
 *    test_video_pipeline_sim <raw_file> <w> <h> <pf> <q> <num_frames> <target_fps> \
 *                             <bw_MBps> <gc_stall_ms> <gc_period_s> [ring_depth]
 *
 *  bw_MBps = 0 means "unlimited" (best-case storage).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#include "../lib/vc5_encoder/gpr_video.h"
#include "../lib/vc5_encoder/gpr_video_format.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void sleep_ms(double ms) {
    if (ms <= 0.0) return;
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000.0);
    ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000.0) * 1000000.0);
    nanosleep(&ts, NULL);
}

typedef struct {
    /* Config */
    double bw_MBps;       /* bandwidth ceiling (MB/s). 0 = unlimited. */
    double gc_stall_ms;   /* extra stall length to simulate GC */
    double gc_period_s;   /* how often to insert a GC stall */
    FILE  *out_fp;        /* optional: write the actual container to disk (NULL = discard) */
    /* State (writer-thread only — no lock needed) */
    double start_ms;
    uint64_t total_bytes;
    uint64_t frame_count;
    double last_gc_ms;
    /* Per-frame instrumentation */
    double t_first_write_ms;
    double t_last_write_ms;
    double total_stall_ms;     /* time spent throttling */
    double total_gc_ms;        /* time spent in GC stalls */
    /* Tag-order verification */
    uint64_t expected_next_tag;   /* next frame_tag we expect to receive */
    uint64_t out_of_order_count;  /* number of out-of-order writer invocations */
} writer_state;

static int throttled_writer(void *user_data, const uint8_t *vc5, size_t size,
                             uint64_t frame_tag)
{
    writer_state *ws = (writer_state *)user_data;
    double t_enter = now_ms();
    if (ws->frame_count == 0) {
        ws->start_ms = t_enter;
        ws->t_first_write_ms = t_enter;
        ws->last_gc_ms = t_enter;
        ws->expected_next_tag = 0;
    }
    /* Verify tag order — caller submits 0..N-1 in order, writer must emit in
       the same order regardless of how many encoders ran in parallel. */
    if (frame_tag != ws->expected_next_tag) {
        ws->out_of_order_count++;
        fprintf(stderr, "  [ORDER ERROR] expected tag %llu, got %llu\n",
                (unsigned long long)ws->expected_next_tag,
                (unsigned long long)frame_tag);
    }
    ws->expected_next_tag = frame_tag + 1;

    /* Emit per-frame header + payload. The container's clip header was
       already written in main() before we ran. The bytes counted toward
       throttle reflect what actually hits storage (header overhead included). */
    uint8_t fh[GPR_VIDEO_FRAME_HEADER_SIZE];
    gpr_video_write_frame_header(fh, sizeof(fh), size, frame_tag);
    if (ws->out_fp) {
        fwrite(fh, 1, sizeof(fh), ws->out_fp);
        fwrite(vc5, 1, size, ws->out_fp);
    }
    ws->total_bytes += sizeof(fh) + size;
    ws->frame_count++;

    /* Bandwidth throttle: ensure cumulative bytes/time ratio stays at or
       below the simulated bus rate. (bw_MBps × 1024 ≈ bytes per ms.) */
    if (ws->bw_MBps > 0.0) {
        double bytes_per_ms     = ws->bw_MBps * 1024.0;      /* MB/s → ~bytes/ms */
        double ideal_elapsed_ms = (double)ws->total_bytes / bytes_per_ms;
        double actual_elapsed_ms = t_enter - ws->start_ms;
        double need_stall_ms = ideal_elapsed_ms - actual_elapsed_ms;
        if (need_stall_ms > 0) {
            sleep_ms(need_stall_ms);
            ws->total_stall_ms += need_stall_ms;
        }
    }

    /* Periodic GC stall (sd cards do this on internal block erase). */
    if (ws->gc_stall_ms > 0 && ws->gc_period_s > 0) {
        double since_last = now_ms() - ws->last_gc_ms;
        if (since_last > ws->gc_period_s * 1000.0) {
            sleep_ms(ws->gc_stall_ms);
            ws->total_gc_ms += ws->gc_stall_ms;
            ws->last_gc_ms = now_ms();
            fprintf(stderr, "  [GC stall at frame %llu]\n",
                    (unsigned long long)frame_tag);
        }
    }

    ws->t_last_write_ms = now_ms();
    (void)vc5; (void)frame_tag;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 11) {
        fprintf(stderr,
            "usage: %s raw w h pf q num_frames target_fps bw_MBps gc_stall_ms gc_period_s [ring_depth=3] [noise_scale] [noise_offset] [denoise_strength] [target_MBps] [out_path] [encoder_count=1]\n"
            "\nstorage profile suggestions:\n"
            "  UHS-I V30 microSD:    bw=80   gc_stall=200 gc_period=8\n"
            "  UHS-I V90 microSD:    bw=150  gc_stall=150 gc_period=10\n"
            "  UHS-II V90 microSD:   bw=200  gc_stall=100 gc_period=15\n"
            "  CFexpress Type A:     bw=700  gc_stall=50  gc_period=30\n"
            "  CFexpress Type B:     bw=1400 gc_stall=30  gc_period=60\n"
            "  unlimited (NVMe-ish): bw=0    gc_stall=0   gc_period=0\n",
            argv[0]);
        return 1;
    }
    const char *raw_path = argv[1];
    int w  = atoi(argv[2]);
    int h  = atoi(argv[3]);
    int pf = atoi(argv[4]);
    int q  = atoi(argv[5]);
    int num_frames    = atoi(argv[6]);
    double target_fps = atof(argv[7]);
    double bw_MBps    = atof(argv[8]);
    double gc_stall   = atof(argv[9]);
    double gc_period  = atof(argv[10]);
    int ring_depth = (argc > 11) ? atoi(argv[11]) : 3;
    double noise_scale  = (argc > 12) ? atof(argv[12]) : 0.0;  /* 0 = no denoise */
    double noise_offset = (argc > 13) ? atof(argv[13]) : 0.0;
    double denoise_strength = (argc > 14) ? atof(argv[14]) : 0.0;
    double target_MBps      = (argc > 15) ? atof(argv[15]) : 0.0;  /* 0 = no rate control */
    const char *out_path    = (argc > 16) ? argv[16] : NULL;       /* NULL = discard */
    int encoder_count       = (argc > 17) ? atoi(argv[17]) : 1;    /* 1 = legacy, 2 = ping-pong */
    if (encoder_count < 1) encoder_count = 1;
    if (encoder_count > 2) encoder_count = 2;

    /* Load one frame; we replay it as if it were a stream. */
    FILE *f = fopen(raw_path, "rb");
    if (!f) { fprintf(stderr, "open %s failed\n", raw_path); return 1; }
    fseek(f, 0, SEEK_END); size_t raw_size = ftell(f); rewind(f);
    uint8_t *raw = (uint8_t *)malloc(raw_size);
    fread(raw, 1, raw_size, f); fclose(f);

    size_t expected = (size_t)w * h * 2;
    if (raw_size < expected) {
        fprintf(stderr, "raw file too small: %zu < %zu\n", raw_size, expected);
        return 1;
    }

    fprintf(stderr, "=== Pipeline simulation ===\n");
    fprintf(stderr, "  source:       %dx%d (%.1f MP), %s\n",
            w, h, (double)w*h/1e6, raw_path);
    fprintf(stderr, "  frames:       %d at %.1f fps target (%.2f ms budget)\n",
            num_frames, target_fps, 1000.0/target_fps);
    fprintf(stderr, "  storage:      %.1f MB/s, GC stalls %.0f ms every %.1f s\n",
            bw_MBps, gc_stall, gc_period);
    fprintf(stderr, "  ring depth:   %d\n", ring_depth);
    fprintf(stderr, "  encoders:     %d %s\n", encoder_count,
            encoder_count == 2 ? "(dual ping-pong)" : "(single)");

    writer_state ws;
    memset(&ws, 0, sizeof(ws));
    ws.bw_MBps = bw_MBps;
    ws.gc_stall_ms = gc_stall;
    ws.gc_period_s = gc_period;
    if (out_path && out_path[0]) {
        ws.out_fp = fopen(out_path, "wb");
        if (!ws.out_fp) {
            fprintf(stderr, "open %s for write failed\n", out_path);
            return 1;
        }
        uint8_t ch[GPR_VIDEO_CLIP_HEADER_SIZE];
        gpr_video_write_clip_header(ch, sizeof(ch), w, h, pf, q,
                                     target_fps, target_MBps,
                                     denoise_strength > 0.0 ? 1 : 0,
                                     (uint32_t)num_frames);
        fwrite(ch, 1, sizeof(ch), ws.out_fp);
        fprintf(stderr, "  output:       %s (clip header + %d frame headers + payloads)\n",
                out_path, num_frames);
    }

    /* Denoise must be set BEFORE first submit. Force split mode since
       wavelet denoise requires the band buffer. */
    if (denoise_strength > 0.0) setenv("FUSED_INLINE_TOKENIZE", "0", 1);

    GPR_VIDEO_ENCODER *enc = gpr_video_encoder_create_dual(
        w, h, pf, q, ring_depth, encoder_count, throttled_writer, &ws);
    if (!enc) { fprintf(stderr, "encoder create failed\n"); return 1; }

    if (denoise_strength > 0.0) {
        gpr_video_encoder_set_denoise(enc, noise_scale, noise_offset, denoise_strength);
        fprintf(stderr, "  denoise:      scale=%g offset=%g strength=%g\n",
                noise_scale, noise_offset, denoise_strength);
    }

    if (target_MBps > 0.0) {
        gpr_video_encoder_set_target_bitrate(enc, target_MBps, target_fps);
        fprintf(stderr, "  rate control: target=%.1f MB/s @ %.1f fps (%.2f MB/frame)\n",
                target_MBps, target_fps, target_MBps / target_fps);
    }

    double frame_interval_ms = 1000.0 / target_fps;
    double sim_start = now_ms();

    double total_submit_ms = 0;
    int submit_blocks = 0;

    for (int i = 0; i < num_frames; i++) {
        double scheduled = sim_start + i * frame_interval_ms;
        double now = now_ms();
        if (now < scheduled) {
            sleep_ms(scheduled - now);
        }
        double t0 = now_ms();
        gpr_video_encoder_submit(enc, raw, expected, (uint64_t)i);
        double t1 = now_ms();
        double submit_dur = t1 - t0;
        total_submit_ms += submit_dur;
        if (submit_dur > 5.0) submit_blocks++;
    }
    double t_after_submit = now_ms();
    fprintf(stderr, "\nAll frames submitted in %.1f ms wall (%.1f ms total submit time)\n",
            t_after_submit - sim_start, total_submit_ms);

    gpr_video_encoder_flush(enc);
    double t_after_flush = now_ms();
    fprintf(stderr, "Flush completed at %.1f ms wall\n", t_after_flush - sim_start);

    gpr_video_stats st;
    gpr_video_encoder_get_stats(enc, &st);

    double wall_s = (t_after_flush - sim_start) / 1000.0;
    double sustained_fps = st.frames_written / wall_s;
    double sustained_mbps = ws.total_bytes / (wall_s * 1024.0 * 1024.0);

    fprintf(stderr, "\n=== Results ===\n");
    fprintf(stderr, "  Submitted:        %llu frames\n", (unsigned long long)st.frames_submitted);
    fprintf(stderr, "  Encoded:          %llu frames\n", (unsigned long long)st.frames_encoded);
    fprintf(stderr, "  Written:          %llu frames\n", (unsigned long long)st.frames_written);
    fprintf(stderr, "  Writer errors:    %llu\n", (unsigned long long)st.writer_errors);
    fprintf(stderr, "  Total bytes:      %llu (%.1f MB)\n",
            (unsigned long long)ws.total_bytes, ws.total_bytes / 1024.0 / 1024.0);
    fprintf(stderr, "  Avg vc5/frame:    %.2f MB\n",
            (ws.total_bytes / (double)num_frames) / 1024.0 / 1024.0);
    fprintf(stderr, "\n  Wall time:        %.1f s\n", wall_s);
    fprintf(stderr, "  Sustained fps:    %.2f\n", sustained_fps);
    fprintf(stderr, "  Sustained out:    %.1f MB/s\n", sustained_mbps);
    fprintf(stderr, "\nPipeline backpressure events:\n");
    fprintf(stderr, "  submit waited:    %llu  (caller blocked on input ring)\n",
            (unsigned long long)st.submit_waited);
    fprintf(stderr, "  encoder waited:   %llu  (encoder blocked on output ring → writer is slow)\n",
            (unsigned long long)st.encoder_waited);
    fprintf(stderr, "  writer waited:    %llu  (writer blocked on empty → encoder is slow)\n",
            (unsigned long long)st.writer_waited);
    fprintf(stderr, "  caller>=5ms blks: %d\n", submit_blocks);
    fprintf(stderr, "  writer stalls:    %.1f ms throttle + %.1f ms GC\n",
            ws.total_stall_ms, ws.total_gc_ms);
    fprintf(stderr, "  out-of-order:     %llu writer invocations %s\n",
            (unsigned long long)ws.out_of_order_count,
            ws.out_of_order_count == 0 ? "(OK — strict tag order preserved)" : "(FAIL)");

    /* Verdict */
    double expected_wall = num_frames / target_fps;
    if (sustained_fps + 0.1 >= target_fps) {
        fprintf(stderr, "\nVERDICT: SUSTAINED target framerate (%.2f >= %.2f fps)\n",
                sustained_fps, target_fps);
    } else {
        fprintf(stderr, "\nVERDICT: UNDER target framerate (%.2f < %.2f fps)\n",
                sustained_fps, target_fps);
        fprintf(stderr, "  Bottleneck: %s\n",
                st.encoder_waited > st.writer_waited ? "WRITER (storage)" : "ENCODER (compute)");
    }

    gpr_video_encoder_destroy(enc);
    if (ws.out_fp) fclose(ws.out_fp);
    free(raw);
    (void)expected_wall;
    return 0;
}
