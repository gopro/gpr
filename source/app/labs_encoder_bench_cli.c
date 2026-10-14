#if !defined(_WIN32)
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

#include "../lib/vc5_encoder/gpr_labs_encoder.h"

typedef struct {
    FILE *fp;
    uint64_t bytes_written;
    uint64_t write_calls;
} writer_ctx;

#if !defined(_WIN32)
typedef struct {
    int fd;
    uint8_t *base;
    size_t mapped_bytes;
    size_t slot_stride;
    int slots;
    size_t frame_bytes;
} mmap_ring_ctx;
#endif

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

#if !defined(_WIN32)
static void sleep_100us(void)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}
#endif

static int parse_int_arg(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (!s || !out) return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v <= 0 || v > 0x7fffffffL) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int env_int(const char *name, int fallback)
{
    const char *s = getenv(name);
    int v = fallback;
    if (s && *s && parse_int_arg(s, &v) == 0) return v;
    return fallback;
}

static uint32_t env_u32(const char *name, uint32_t fallback)
{
    const char *s = getenv(name);
    char *end = NULL;
    unsigned long v;
    if (!s || !*s) return fallback;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > 0xfffffffful) return fallback;
    return (uint32_t)v;
}

static double env_double(const char *name, double fallback)
{
    const char *s = getenv(name);
    char *end = NULL;
    double v;
    if (!s || !*s) return fallback;
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || v <= 0.0) return fallback;
    return v;
}

static int read_exact_frame(const char *path, uint8_t *dst, size_t bytes)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "open raw input failed: %s\n", path);
        return -1;
    }
    size_t n = fread(dst, 1, bytes, fp);
    if (n != bytes) {
        fprintf(stderr, "raw input too small: read %zu expected %zu\n", n, bytes);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

static int read_exact_stream(FILE *fp, uint8_t *dst, size_t bytes)
{
    size_t got = 0;
    while (got < bytes) {
        size_t n = fread(dst + got, 1, bytes - got, fp);
        if (n == 0) {
            if (ferror(fp)) {
                perror("stream raw input read failed");
            }
            return -1;
        }
        got += n;
    }
    return 0;
}

static int env_bool(const char *name)
{
    const char *s = getenv(name);
    return (s && *s && strcmp(s, "0") != 0) ? 1 : 0;
}

#if !defined(_WIN32)
static uint64_t load_u64_le_volatile(const volatile uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | (uint64_t)p[i];
    }
    return v;
}

static void store_u64_le_volatile(volatile uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xffu);
    }
}

static void mmap_ring_close(mmap_ring_ctx *ring)
{
    if (!ring) return;
    if (ring->base && ring->base != MAP_FAILED) {
        munmap(ring->base, ring->mapped_bytes);
    }
    if (ring->fd >= 0) close(ring->fd);
    memset(ring, 0, sizeof(*ring));
    ring->fd = -1;
}

static int mmap_ring_open(mmap_ring_ctx *ring, const char *path, size_t frame_bytes)
{
    if (!ring || !path || frame_bytes == 0) return -1;
    memset(ring, 0, sizeof(*ring));
    ring->fd = -1;
    ring->slots = env_int("GPR_LABS_MMAP_RING_SLOTS", 3);
    if (ring->slots <= 0) ring->slots = 3;
    ring->frame_bytes = frame_bytes;
    ring->slot_stride = 64u + frame_bytes;
    ring->mapped_bytes = ring->slot_stride * (size_t)ring->slots;
    ring->fd = open(path, O_RDWR);
    if (ring->fd < 0) {
        fprintf(stderr, "open mmap ring input failed: %s\n", path);
        return -1;
    }
    struct stat st;
    if (fstat(ring->fd, &st) != 0 || st.st_size < (off_t)ring->mapped_bytes) {
        fprintf(stderr, "mmap ring input too small: %s\n", path);
        mmap_ring_close(ring);
        return -1;
    }
    ring->base = (uint8_t *)mmap(NULL, ring->mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ring->fd, 0);
    if (ring->base == MAP_FAILED) {
        fprintf(stderr, "mmap ring input failed: %s\n", path);
        mmap_ring_close(ring);
        return -1;
    }
    return 0;
}

static const uint8_t *mmap_ring_wait_frame(mmap_ring_ctx *ring, int frame_index)
{
    int slot = frame_index % ring->slots;
    uint8_t *slot_base = ring->base + (size_t)slot * ring->slot_stride;
    volatile uint8_t *ready_ptr = (volatile uint8_t *)slot_base;
    uint64_t want = (uint64_t)frame_index + 1u;
    while (load_u64_le_volatile(ready_ptr) != want) {
        sleep_100us();
    }
    return slot_base + 64u;
}

static void mmap_ring_mark_consumed(mmap_ring_ctx *ring, int frame_index)
{
    int slot = frame_index % ring->slots;
    uint8_t *slot_base = ring->base + (size_t)slot * ring->slot_stride;
    store_u64_le_volatile((volatile uint8_t *)(slot_base + 16u), (uint64_t)frame_index + 1u);
}
#endif

static int write_cb(void *user, const uint8_t *data, size_t size)
{
    writer_ctx *ctx = (writer_ctx *)user;
    if (!ctx || !ctx->fp || !data) return -1;
    if (fwrite(data, 1, size, ctx->fp) != size) return -1;
    ctx->bytes_written += (uint64_t)size;
    ctx->write_calls++;
    return 0;
}

static int mul_size(size_t a, size_t b, size_t *out)
{
    if (!out) return -1;
    if (a != 0 && b > ((size_t)-1) / a) return -1;
    *out = a * b;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s RAW WIDTH HEIGHT FRAMES\n", argv[0]);
        return 2;
    }

    const char *raw_path = argv[1];
    int width = 0, height = 0, frames = 0;
    if (parse_int_arg(argv[2], &width) != 0 ||
        parse_int_arg(argv[3], &height) != 0 ||
        parse_int_arg(argv[4], &frames) != 0) {
        fprintf(stderr, "invalid WIDTH HEIGHT or FRAMES\n");
        return 2;
    }

    const char *gvid_path = getenv("GPR_BENCH_GVID");
    if (!gvid_path || !*gvid_path) {
        fprintf(stderr, "GPR_BENCH_GVID must name the output .gvid path\n");
        return 2;
    }

    size_t row_bytes = 0, raw_bytes = 0;
    if (mul_size((size_t)width, 2u, &row_bytes) != 0 ||
        mul_size(row_bytes, (size_t)height, &raw_bytes) != 0) {
        fprintf(stderr, "raw size overflow\n");
        return 2;
    }

    uint32_t stride = env_u32("GPR_LABS_STRIDE_BYTES", (uint32_t)row_bytes);
    if (stride < row_bytes) {
        fprintf(stderr, "GPR_LABS_STRIDE_BYTES must be >= WIDTH*2\n");
        return 2;
    }
    size_t frame_bytes = 0;
    if (mul_size((size_t)stride, (size_t)height, &frame_bytes) != 0) {
        fprintf(stderr, "stride size overflow\n");
        return 2;
    }

    int stream_input = env_bool("GPR_LABS_STREAM_INPUT");
    int mmap_ring_input = env_bool("GPR_LABS_MMAP_RING_INPUT");
    if (stream_input && mmap_ring_input) {
        fprintf(stderr, "GPR_LABS_STREAM_INPUT and GPR_LABS_MMAP_RING_INPUT are mutually exclusive\n");
        return 2;
    }
#if defined(_WIN32)
    if (mmap_ring_input) {
        fprintf(stderr, "GPR_LABS_MMAP_RING_INPUT is not supported on this platform\n");
        return 2;
    }
#endif
    uint8_t *frame = (uint8_t *)malloc(frame_bytes);
    if (!frame) {
        fprintf(stderr, "frame allocation failed\n");
        return 1;
    }
    memset(frame, 0, frame_bytes);
    if (!stream_input && !mmap_ring_input && read_exact_frame(raw_path, frame, raw_bytes) != 0) {
        free(frame);
        return 1;
    }

    FILE *stream_fp = NULL;
    if (stream_input) {
        stream_fp = fopen(raw_path, "rb");
        if (!stream_fp) {
            fprintf(stderr, "open stream raw input failed: %s\n", raw_path);
            free(frame);
            return 1;
        }
        fprintf(stderr, "# GPR_LABS_STREAM_INPUT=1 - reading one frame per submit from %s\n", raw_path);
    }

#if !defined(_WIN32)
    mmap_ring_ctx mmap_ring;
    memset(&mmap_ring, 0, sizeof mmap_ring);
    mmap_ring.fd = -1;
    if (mmap_ring_input) {
        if (mmap_ring_open(&mmap_ring, raw_path, frame_bytes) != 0) {
            if (stream_fp) fclose(stream_fp);
            free(frame);
            return 1;
        }
        fprintf(stderr, "# GPR_LABS_MMAP_RING_INPUT=1 - reading mapped DMA-ring slots from %s slots=%d\n",
                raw_path, mmap_ring.slots);
    }
#endif

    FILE *fp = fopen(gvid_path, "wb");
    if (!fp) {
        fprintf(stderr, "open GPR_BENCH_GVID failed: %s\n", gvid_path);
        if (stream_fp) fclose(stream_fp);
#if !defined(_WIN32)
        if (mmap_ring_input) mmap_ring_close(&mmap_ring);
#endif
        free(frame);
        return 1;
    }
    writer_ctx writer = { fp, 0, 0 };

    double fps = env_double("GPR_BENCH_GVID_FPS", 24.0);
    gpr_labs_encoder_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.width = (uint32_t)width;
    cfg.height = (uint32_t)height;
    cfg.stride_bytes = stride;
    cfg.bit_depth = (uint16_t)env_int("GPR_LABS_BIT_DEPTH", 16);
    cfg.pixel_format = (uint16_t)env_int("GPR_BENCH_PIXEL_FORMAT", 4);
    cfg.quality = (uint16_t)env_int("FUSED_QUALITY", 3);
    cfg.fps_x1000 = (uint32_t)(fps * 1000.0 + 0.5);
    cfg.target_kbps = env_u32("GPR_LABS_TARGET_KBPS", 0);
    cfg.max_inflight_frames = env_u32("GPR_LABS_MAX_INFLIGHT", 3);

    gpr_labs_encoder *enc = gpr_labs_encoder_create(&cfg, write_cb, &writer);
    if (!enc) {
        fprintf(stderr, "gpr_labs_encoder_create failed\n");
        fclose(fp);
        if (stream_fp) fclose(stream_fp);
#if !defined(_WIN32)
        if (mmap_ring_input) mmap_ring_close(&mmap_ring);
#endif
        free(frame);
        return 1;
    }

    double t_start = now_ms();
    for (int i = 0; i < frames; i++) {
        double read_ms = 0.0;
        const uint8_t *submit_data = frame;
        if (stream_input) {
            double r0 = now_ms();
            if (read_exact_stream(stream_fp, frame, frame_bytes) != 0) {
                fprintf(stderr, "stream raw input ended before frame %d\n", i);
                gpr_labs_encoder_cancel(enc);
                gpr_labs_encoder_destroy(enc);
                fclose(fp);
                if (stream_fp) fclose(stream_fp);
#if !defined(_WIN32)
                if (mmap_ring_input) mmap_ring_close(&mmap_ring);
#endif
                free(frame);
                return 1;
            }
            read_ms = now_ms() - r0;
        }
#if !defined(_WIN32)
        if (mmap_ring_input) {
            double r0 = now_ms();
            submit_data = mmap_ring_wait_frame(&mmap_ring, i);
            read_ms = now_ms() - r0;
        }
#endif
        gpr_labs_frame f;
        memset(&f, 0, sizeof f);
        f.data = submit_data;
        f.size_bytes = frame_bytes;
        f.frame_index = (uint64_t)i;
        f.timestamp_ns = (uint64_t)((1000000000.0 / fps) * (double)i);

        double t0 = now_ms();
        if (gpr_labs_encoder_submit(enc, &f) != 0) {
            fprintf(stderr, "gpr_labs_encoder_submit failed at frame %d\n", i);
            gpr_labs_encoder_cancel(enc);
            gpr_labs_encoder_destroy(enc);
            fclose(fp);
            if (stream_fp) fclose(stream_fp);
#if !defined(_WIN32)
            if (mmap_ring_input) mmap_ring_close(&mmap_ring);
#endif
            free(frame);
            return 1;
        }
#if !defined(_WIN32)
        if (mmap_ring_input) {
            mmap_ring_mark_consumed(&mmap_ring, i);
        }
#endif
        double t1 = now_ms();
        if (stream_input || mmap_ring_input) {
            printf("# stream_frame frame=%d source_read_ms=%.3f submit_ms=%.3f\n", i, read_ms, t1 - t0);
        } else {
            printf("%.3f\n", t1 - t0);
        }
    }

    double flush0 = now_ms();
    if (gpr_labs_encoder_flush(enc) != 0) {
        fprintf(stderr, "gpr_labs_encoder_flush failed\n");
        gpr_labs_encoder_destroy(enc);
        fclose(fp);
        if (stream_fp) fclose(stream_fp);
#if !defined(_WIN32)
        if (mmap_ring_input) mmap_ring_close(&mmap_ring);
#endif
        free(frame);
        return 1;
    }
    double flush1 = now_ms();
    double total_ms = flush1 - t_start;

    gpr_video_stats stats;
    memset(&stats, 0, sizeof stats);
    gpr_labs_encoder_get_stats(enc, &stats);
    gpr_labs_encoder_destroy(enc);
    fclose(fp);
    if (stream_fp) fclose(stream_fp);
#if !defined(_WIN32)
    if (mmap_ring_input) mmap_ring_close(&mmap_ring);
#endif
    free(frame);

    fprintf(stderr, "# bench_phase_ms async_drain n=1 mean=%.3f stddev=0.000 min=%.3f p25=%.3f median=%.3f p75=%.3f p95=%.3f p99=%.3f max=%.3f\n",
            flush1 - flush0, flush1 - flush0, flush1 - flush0, flush1 - flush0,
            flush1 - flush0, flush1 - flush0, flush1 - flush0, flush1 - flush0);
    fprintf(stderr, "# labs_encoder_stats submitted=%llu encoded=%llu written=%llu writer_errors=%llu write_calls=%llu bytes=%llu\n",
            (unsigned long long)stats.frames_submitted,
            (unsigned long long)stats.frames_encoded,
            (unsigned long long)stats.frames_written,
            (unsigned long long)stats.writer_errors,
            (unsigned long long)writer.write_calls,
            (unsigned long long)writer.bytes_written);

    return (stats.frames_written == (uint64_t)frames && stats.writer_errors == 0) ? 0 : 1;
}
