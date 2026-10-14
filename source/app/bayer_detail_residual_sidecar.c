#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    BDRS_MAGIC = 0x31535244, /* DRS1 little-endian on disk after leading 'B' version field below */
    BDRS_VERSION = 1,
    BDRC_VERSION = 2,
};

typedef struct BDRSHeader {
    char magic[4]; /* BDRS */
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t plane_mask;
    int32_t significant_detail_threshold;
    int32_t residual_threshold;
    int32_t quant_step;
    uint32_t max_value;
    uint64_t total_plane_samples;
    uint64_t bitmap_bytes;
    uint64_t value_count;
} BDRSHeader;

typedef struct BDRCHeader {
    char magic[4]; /* BDRC */
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t plane_mask;
    int32_t significant_detail_threshold;
    int32_t residual_threshold;
    int32_t quant_step;
    uint32_t max_value;
    uint64_t total_plane_samples;
    uint64_t bitmap_bytes;
    uint64_t value_count;
    uint64_t value_payload_bytes;
} BDRCHeader;

typedef struct ByteBuffer {
    uint8_t *data;
    size_t size;
    size_t cap;
} ByteBuffer;

typedef struct EncodeJob {
    const uint16_t *codec;
    const uint16_t *clean;
    int16_t *qdense;
    uint32_t width;
    uint32_t plane_w;
    uint32_t plane_h;
    uint32_t plane_mask;
    int32_t sig;
    int32_t resid;
    int32_t qstep;
    uint32_t row_begin;
    uint32_t row_end;
} EncodeJob;

typedef struct CompactEncodeJob {
    const uint16_t *codec;
    const uint16_t *clean;
    uint8_t *bitmap;
    ByteBuffer plane_payloads[4];
    uint64_t value_count;
    uint32_t width;
    uint32_t plane_w;
    uint32_t plane_h;
    uint32_t plane_mask;
    uint64_t bitmap_bytes;
    int32_t sig;
    int32_t resid;
    int32_t qstep;
    uint32_t row_begin;
    uint32_t row_end;
    int failed;
} CompactEncodeJob;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;
    if (!s || !out) return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || !end || *end || v > 0xffffffffUL) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_i32(const char *s, int32_t *out)
{
    char *end = NULL;
    long v;
    if (!s || !out) return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < -0x7fffffffL || v > 0x7fffffffL) return -1;
    *out = (int32_t)v;
    return 0;
}

static int parse_env_threads(void)
{
    const char *s = getenv("BDRS_ENCODE_THREADS");
    char *end = NULL;
    long v;
    if (!s || !*s) return 1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end || v < 1) return 1;
    if (v > 64) return 64;
    return (int)v;
}

static int parse_env_bool(const char *name)
{
    const char *s = getenv(name);
    return s && (*s == '1' || *s == 'y' || *s == 'Y' || *s == 't' || *s == 'T');
}

static int byte_buffer_reserve(ByteBuffer *b, size_t extra)
{
    if (extra > SIZE_MAX - b->size) return -1;
    size_t need = b->size + extra;
    if (need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    uint8_t *next = (uint8_t *)realloc(b->data, cap);
    if (!next) return -1;
    b->data = next;
    b->cap = cap;
    return 0;
}

static int byte_buffer_put(ByteBuffer *b, uint8_t value)
{
    if (byte_buffer_reserve(b, 1) != 0) return -1;
    b->data[b->size++] = value;
    return 0;
}

static uint32_t zigzag_encode_i32(int32_t value)
{
    return ((uint32_t)value << 1) ^ (uint32_t)(value >> 31);
}

static int32_t zigzag_decode_u32(uint32_t value)
{
    return (int32_t)((value >> 1) ^ (uint32_t)-(int32_t)(value & 1u));
}

static int varint_put_u32(ByteBuffer *b, uint32_t value)
{
    while (value >= 0x80u) {
        if (byte_buffer_put(b, (uint8_t)((value & 0x7fu) | 0x80u)) != 0) return -1;
        value >>= 7;
    }
    return byte_buffer_put(b, (uint8_t)value);
}

static int varint_get_u32(const uint8_t *data, size_t size, size_t *pos, uint32_t *out)
{
    uint32_t value = 0;
    uint32_t shift = 0;
    while (*pos < size && shift <= 28) {
        uint8_t byte = data[(*pos)++];
        value |= (uint32_t)(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            *out = value;
            return 0;
        }
        shift += 7;
    }
    return -1;
}

static int encode_compact_values(ByteBuffer *payload, const int16_t *values,
                                 uint64_t value_count, int32_t quant_step)
{
    for (uint64_t i = 0; i < value_count; i++) {
        int32_t q = (int32_t)values[i];
        int32_t unit = q / quant_step;
        if (unit * quant_step != q) return -1;
        if (varint_put_u32(payload, zigzag_encode_i32(unit)) != 0) return -1;
    }
    return 0;
}

static int read_file_exact(const char *path, void *dst, size_t bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    size_t n = fread(dst, 1, bytes, f);
    int err = ferror(f);
    fclose(f);
    if (err || n != bytes) {
        fprintf(stderr, "read %s failed: got %zu bytes, expected %zu\n", path, n, bytes);
        return -1;
    }
    return 0;
}

static int write_file_exact(const char *path, const void *src, size_t bytes)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    size_t n = fwrite(src, 1, bytes, f);
    int err = ferror(f);
    if (fclose(f) != 0) err = 1;
    if (err || n != bytes) {
        fprintf(stderr, "write %s failed: wrote %zu bytes, expected %zu\n", path, n, bytes);
        return -1;
    }
    return 0;
}

static int reflect_idx(int i, int n)
{
    if (n <= 1) return 0;
    if (i < 0) return -i;
    if (i >= n) return 2 * n - 2 - i;
    return i;
}

static size_t raw_index(uint32_t width, int plane, int py, int px)
{
    int yoff = (plane >= 2) ? 1 : 0;
    int xoff = (plane == 1 || plane == 3) ? 1 : 0;
    return (size_t)(py * 2 + yoff) * (size_t)width + (size_t)(px * 2 + xoff);
}

static void detail_terms3_plane_x16(const uint16_t *codec, const uint16_t *clean,
                                    uint32_t width, uint32_t plane_w, uint32_t plane_h,
                                    int plane, int py, int px,
                                    int32_t *clean_detail_x16, int32_t *detail_residual_x16)
{
    static const int k[3][3] = {{1, 2, 1}, {2, 4, 2}, {1, 2, 1}};
    int clean_sum = 0;
    int diff_sum = 0;
    size_t center = raw_index(width, plane, py, px);
    int center_clean = (int)clean[center];
    int center_diff = center_clean - (int)codec[center];
    for (int dy = -1; dy <= 1; dy++) {
        int yy = reflect_idx(py + dy, (int)plane_h);
        for (int dx = -1; dx <= 1; dx++) {
            int xx = reflect_idx(px + dx, (int)plane_w);
            int w = k[dy + 1][dx + 1];
            size_t idx = raw_index(width, plane, yy, xx);
            int c = (int)codec[idx];
            int cl = (int)clean[idx];
            clean_sum += w * cl;
            diff_sum += w * (cl - c);
        }
    }
    *clean_detail_x16 = 16 * center_clean - clean_sum;
    *detail_residual_x16 = 16 * center_diff - diff_sum;
}

static void detail_terms3_plane_interior_x16(const uint16_t *codec, const uint16_t *clean,
                                             uint32_t width, int plane, int py, int px,
                                             int32_t *clean_detail_x16,
                                             int32_t *detail_residual_x16)
{
    size_t center = raw_index(width, plane, py, px);
    size_t row = (size_t)width * 2u;
    const size_t nw = center - row - 2u;
    const size_t n = center - row;
    const size_t ne = center - row + 2u;
    const size_t w = center - 2u;
    const size_t e = center + 2u;
    const size_t sw = center + row - 2u;
    const size_t s = center + row;
    const size_t se = center + row + 2u;
    int center_clean = (int)clean[center];
    int center_diff = center_clean - (int)codec[center];
    int clean_sum =
        (int)clean[nw] + 2 * (int)clean[n] + (int)clean[ne] +
        2 * (int)clean[w] + 4 * center_clean + 2 * (int)clean[e] +
        (int)clean[sw] + 2 * (int)clean[s] + (int)clean[se];
    int diff_sum =
        ((int)clean[nw] - (int)codec[nw]) +
        2 * ((int)clean[n] - (int)codec[n]) +
        ((int)clean[ne] - (int)codec[ne]) +
        2 * ((int)clean[w] - (int)codec[w]) +
        4 * center_diff +
        2 * ((int)clean[e] - (int)codec[e]) +
        ((int)clean[sw] - (int)codec[sw]) +
        2 * ((int)clean[s] - (int)codec[s]) +
        ((int)clean[se] - (int)codec[se]);
    *clean_detail_x16 = 16 * center_clean - clean_sum;
    *detail_residual_x16 = 16 * center_diff - diff_sum;
}

static int64_t abs_i64(int64_t value)
{
    return value < 0 ? -value : value;
}

static int32_t round_div_nearest_even_i64(int64_t numerator, int64_t denominator)
{
    int sign = 1;
    if (numerator < 0) {
        numerator = -numerator;
        sign = -1;
    }
    int64_t q = numerator / denominator;
    int64_t r = numerator % denominator;
    int cmp = (r * 2 > denominator) || (r * 2 == denominator && (q & 1));
    if (cmp) q++;
    if (sign < 0) q = -q;
    if (q < INT32_MIN) return INT32_MIN;
    if (q > INT32_MAX) return INT32_MAX;
    return (int32_t)q;
}

static int32_t quantize_residual_x16(int32_t residual_x16, int32_t quant_step)
{
    if (quant_step <= 0) return 0;
    int64_t denom = 16LL * (int64_t)quant_step;
    int32_t units = round_div_nearest_even_i64((int64_t)residual_x16, denom);
    int64_t q = (int64_t)units * (int64_t)quant_step;
    if (q < INT32_MIN) return INT32_MIN;
    if (q > INT32_MAX) return INT32_MAX;
    return (int32_t)q;
}

static void bitmap_set(uint8_t *bitmap, uint64_t idx)
{
    bitmap[idx >> 3] |= (uint8_t)(1u << (7u - (uint32_t)(idx & 7u)));
}

static int bitmap_get(const uint8_t *bitmap, uint64_t idx)
{
    return (bitmap[idx >> 3] >> (7u - (uint32_t)(idx & 7u))) & 1u;
}

static int16_t compute_q_sample(const uint16_t *codec, const uint16_t *clean,
                                uint32_t width, uint32_t plane_w, uint32_t plane_h,
                                int plane, uint32_t py, uint32_t px,
                                uint32_t plane_mask, int32_t sig, int32_t resid,
                                int32_t qstep)
{
    int32_t q = 0;
    if (plane_mask & (1u << plane)) {
        int32_t clean_detail_x16 = 0;
        int32_t r_x16 = 0;
        if (py > 0 && px > 0 && py + 1u < plane_h && px + 1u < plane_w) {
            detail_terms3_plane_interior_x16(codec, clean, width, plane,
                                             (int)py, (int)px, &clean_detail_x16, &r_x16);
        } else {
            detail_terms3_plane_x16(codec, clean, width, plane_w, plane_h, plane,
                                    (int)py, (int)px, &clean_detail_x16, &r_x16);
        }
        if (abs_i64(clean_detail_x16) >= (int64_t)sig * 16LL &&
            abs_i64(r_x16) >= (int64_t)resid * 16LL) {
            q = quantize_residual_x16(r_x16, qstep);
        }
    }
    if (q < -32768) q = -32768;
    if (q > 32767) q = 32767;
    return (int16_t)q;
}

static void *encode_worker(void *opaque)
{
    EncodeJob *job = (EncodeJob *)opaque;
    uint64_t plane_samples = (uint64_t)job->plane_w * (uint64_t)job->plane_h;
    for (int plane = 0; plane < 4; plane++) {
        uint64_t plane_base = (uint64_t)plane * plane_samples;
        for (uint32_t py = job->row_begin; py < job->row_end; py++) {
            uint64_t row_base = plane_base + (uint64_t)py * (uint64_t)job->plane_w;
            for (uint32_t px = 0; px < job->plane_w; px++) {
                job->qdense[row_base + px] =
                    compute_q_sample(job->codec, job->clean, job->width,
                                     job->plane_w, job->plane_h, plane, py, px,
                                     job->plane_mask, job->sig, job->resid,
                                     job->qstep);
            }
        }
    }
    return NULL;
}

static void compact_encode_job_destroy(CompactEncodeJob *job)
{
    if (!job) return;
    free(job->bitmap);
    for (int plane = 0; plane < 4; plane++) {
        free(job->plane_payloads[plane].data);
    }
}

static int compact_payload_put_q(ByteBuffer *payload, int16_t q, int32_t quant_step)
{
    int32_t value = (int32_t)q;
    int32_t unit = value / quant_step;
    if (unit * quant_step != value) return -1;
    return varint_put_u32(payload, zigzag_encode_i32(unit));
}

static void *compact_encode_worker(void *opaque)
{
    CompactEncodeJob *job = (CompactEncodeJob *)opaque;
    uint64_t plane_samples = (uint64_t)job->plane_w * (uint64_t)job->plane_h;
    job->bitmap = (uint8_t *)calloc((size_t)job->bitmap_bytes, 1);
    if (!job->bitmap) {
        job->failed = 1;
        return NULL;
    }
    for (int plane = 0; plane < 4; plane++) {
        uint64_t plane_base = (uint64_t)plane * plane_samples;
        for (uint32_t py = job->row_begin; py < job->row_end; py++) {
            uint64_t row_base = plane_base + (uint64_t)py * (uint64_t)job->plane_w;
            for (uint32_t px = 0; px < job->plane_w; px++) {
                int16_t q = compute_q_sample(job->codec, job->clean, job->width,
                                             job->plane_w, job->plane_h, plane, py, px,
                                             job->plane_mask, job->sig, job->resid,
                                             job->qstep);
                if (q == 0) continue;
                bitmap_set(job->bitmap, row_base + px);
                if (compact_payload_put_q(&job->plane_payloads[plane], q, job->qstep) != 0) {
                    job->failed = 1;
                    return NULL;
                }
                job->value_count++;
            }
        }
    }
    return NULL;
}

static int append_bytes(ByteBuffer *dst, const uint8_t *src, size_t size)
{
    if (size == 0) return 0;
    if (byte_buffer_reserve(dst, size) != 0) return -1;
    memcpy(dst->data + dst->size, src, size);
    dst->size += size;
    return 0;
}

static int write_receipt(const char *path, const char *cmd, const BDRSHeader *h,
                         uint64_t sidecar_bytes, double elapsed_ms,
                         double codec_clean_rmse, double output_clean_rmse,
                         int encode_threads, const char *sidecar_format,
                         uint64_t value_payload_bytes)
{
    if (!path) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "open receipt %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f,
            "{\n"
            "  \"schema\": \"gpr.bayer_detail_residual_sidecar_native.v1\",\n"
            "  \"cmd\": \"%s\",\n"
            "  \"width\": %u,\n"
            "  \"height\": %u,\n"
            "  \"plane_mask\": %u,\n"
            "  \"significant_detail_threshold\": %d,\n"
            "  \"residual_threshold\": %d,\n"
            "  \"quant_step\": %d,\n"
            "  \"max_value\": %u,\n"
            "  \"total_plane_samples\": %llu,\n"
            "  \"bitmap_bytes\": %llu,\n"
            "  \"value_count\": %llu,\n"
            "  \"value_payload_bytes\": %llu,\n"
            "  \"sidecar_bytes\": %llu,\n"
            "  \"sidecar_format\": \"%s\",\n"
            "  \"elapsed_ms\": %.6f,\n"
            "  \"encode_threads\": %d,\n"
            "  \"codec_clean_rmse\": %.9f,\n"
            "  \"output_clean_rmse\": %.9f\n"
            "}\n",
            cmd, h->width, h->height, h->plane_mask,
            h->significant_detail_threshold, h->residual_threshold, h->quant_step,
            h->max_value, (unsigned long long)h->total_plane_samples,
            (unsigned long long)h->bitmap_bytes, (unsigned long long)h->value_count,
            (unsigned long long)value_payload_bytes,
            (unsigned long long)sidecar_bytes, sidecar_format, elapsed_ms, encode_threads,
            codec_clean_rmse, output_clean_rmse);
    if (fclose(f) != 0) {
        fprintf(stderr, "close receipt %s failed\n", path);
        return -1;
    }
    return 0;
}

static double rmse_raw(const uint16_t *a, const uint16_t *b, uint64_t count)
{
    long double acc = 0.0;
    for (uint64_t i = 0; i < count; i++) {
        long double d = (long double)a[i] - (long double)b[i];
        acc += d * d;
    }
    return sqrt((double)(acc / (long double)count));
}

static int encode_cmd(int argc, char **argv)
{
    if (argc != 12 && argc != 13) {
        fprintf(stderr,
                "usage: %s encode <codec.raw> <clean.raw> <sidecar.bdrs> <width> <height> <plane_mask> <sig_thresh> <resid_thresh> <quant_step> <max_value> [receipt.json]\n",
                argv[0]);
        return 2;
    }
    const char *codec_path = argv[2];
    const char *clean_path = argv[3];
    const char *sidecar_path = argv[4];
    const char *receipt_path = argc == 13 ? argv[12] : NULL;
    uint32_t width = 0, height = 0, plane_mask = 0, max_value = 0;
    int32_t sig = 0, resid = 0, qstep = 0;
    if (parse_u32(argv[5], &width) || parse_u32(argv[6], &height) ||
        parse_u32(argv[7], &plane_mask) || parse_i32(argv[8], &sig) ||
        parse_i32(argv[9], &resid) || parse_i32(argv[10], &qstep) ||
        parse_u32(argv[11], &max_value) || width == 0 || height == 0 ||
        (width & 1u) || (height & 1u) || qstep <= 0 || max_value == 0) {
        fprintf(stderr, "invalid encode arguments\n");
        return 2;
    }

    uint64_t raw_count = (uint64_t)width * (uint64_t)height;
    size_t raw_bytes = (size_t)raw_count * sizeof(uint16_t);
    uint16_t *codec = (uint16_t *)malloc(raw_bytes);
    uint16_t *clean = (uint16_t *)malloc(raw_bytes);
    if (!codec || !clean) {
        fprintf(stderr, "raw allocation failed\n");
        free(codec);
        free(clean);
        return 1;
    }
    if (read_file_exact(codec_path, codec, raw_bytes) || read_file_exact(clean_path, clean, raw_bytes)) {
        free(codec);
        free(clean);
        return 1;
    }

    double t0 = now_ms();
    uint32_t plane_w = width / 2u;
    uint32_t plane_h = height / 2u;
    uint64_t total_plane_samples = (uint64_t)plane_w * (uint64_t)plane_h * 4u;
    uint64_t bitmap_bytes = (total_plane_samples + 7u) / 8u;
    int compact = parse_env_bool("BDRS_COMPACT");
    int encode_threads = parse_env_threads();
    if ((uint32_t)encode_threads > plane_h) encode_threads = (int)plane_h;
    int direct_compact = compact && encode_threads > 1;
    ByteBuffer compact_payload = {0};
    uint8_t *bitmap = (uint8_t *)calloc((size_t)bitmap_bytes, 1);
    int16_t *values = NULL;
    if (!direct_compact) {
        values = (int16_t *)malloc((size_t)total_plane_samples * sizeof(int16_t));
    }
    if (!bitmap || (!direct_compact && !values)) {
        fprintf(stderr, "sidecar allocation failed\n");
        free(codec); free(clean); free(bitmap); free(values);
        return 1;
    }

    uint64_t value_count = 0;
    uint64_t sample_idx = 0;
    if (direct_compact) {
        pthread_t *threads = (pthread_t *)calloc((size_t)encode_threads, sizeof(pthread_t));
        CompactEncodeJob *jobs = (CompactEncodeJob *)calloc((size_t)encode_threads, sizeof(CompactEncodeJob));
        int created = 0;
        if (!threads || !jobs) {
            fprintf(stderr, "parallel compact encode allocation failed\n");
            free(codec); free(clean); free(bitmap); free(values);
            free(threads); free(jobs);
            return 1;
        }
        for (int t = 0; t < encode_threads; t++) {
            uint32_t row_begin = (uint32_t)(((uint64_t)t * plane_h) / (uint64_t)encode_threads);
            uint32_t row_end = (uint32_t)(((uint64_t)(t + 1) * plane_h) / (uint64_t)encode_threads);
            jobs[t].codec = codec;
            jobs[t].clean = clean;
            jobs[t].width = width;
            jobs[t].plane_w = plane_w;
            jobs[t].plane_h = plane_h;
            jobs[t].plane_mask = plane_mask;
            jobs[t].bitmap_bytes = bitmap_bytes;
            jobs[t].sig = sig;
            jobs[t].resid = resid;
            jobs[t].qstep = qstep;
            jobs[t].row_begin = row_begin;
            jobs[t].row_end = row_end;
            if (pthread_create(&threads[t], NULL, compact_encode_worker, &jobs[t]) != 0) {
                fprintf(stderr, "pthread_create failed\n");
                for (int j = 0; j < created; j++) {
                    pthread_join(threads[j], NULL);
                }
                for (int j = 0; j < encode_threads; j++) {
                    compact_encode_job_destroy(&jobs[j]);
                }
                free(codec); free(clean); free(bitmap); free(values); free(compact_payload.data);
                free(threads); free(jobs);
                return 1;
            }
            created++;
        }
        int compact_failed = 0;
        for (int t = 0; t < encode_threads; t++) {
            if (pthread_join(threads[t], NULL) != 0) {
                fprintf(stderr, "pthread_join failed\n");
                compact_failed = 1;
            }
            if (jobs[t].failed) {
                compact_failed = 1;
            }
            value_count += jobs[t].value_count;
        }
        if (!compact_failed) {
            for (int t = 0; t < encode_threads; t++) {
                for (uint64_t i = 0; i < bitmap_bytes; i++) {
                    bitmap[i] |= jobs[t].bitmap[i];
                }
            }
            for (int plane = 0; plane < 4 && !compact_failed; plane++) {
                for (int t = 0; t < encode_threads; t++) {
                    ByteBuffer *src = &jobs[t].plane_payloads[plane];
                    if (append_bytes(&compact_payload, src->data, src->size) != 0) {
                        compact_failed = 1;
                        break;
                    }
                }
            }
        }
        for (int t = 0; t < encode_threads; t++) {
            compact_encode_job_destroy(&jobs[t]);
        }
        free(threads);
        free(jobs);
        if (compact_failed) {
            fprintf(stderr, "parallel compact encode failed\n");
            free(codec); free(clean); free(bitmap); free(values); free(compact_payload.data);
            return 1;
        }
    } else if (encode_threads <= 1) {
        for (int plane = 0; plane < 4; plane++) {
            for (uint32_t py = 0; py < plane_h; py++) {
                for (uint32_t px = 0; px < plane_w; px++, sample_idx++) {
                    int16_t q = compute_q_sample(codec, clean, width, plane_w, plane_h,
                                                 plane, py, px, plane_mask, sig, resid, qstep);
                    if (q != 0) {
                        bitmap_set(bitmap, sample_idx);
                        values[value_count++] = q;
                    }
                }
            }
        }
    } else {
        int16_t *qdense = (int16_t *)calloc((size_t)total_plane_samples, sizeof(int16_t));
        pthread_t *threads = (pthread_t *)calloc((size_t)encode_threads, sizeof(pthread_t));
        EncodeJob *jobs = (EncodeJob *)calloc((size_t)encode_threads, sizeof(EncodeJob));
        if (!qdense || !threads || !jobs) {
            fprintf(stderr, "parallel encode allocation failed\n");
            free(codec); free(clean); free(bitmap); free(values);
            free(qdense); free(threads); free(jobs);
            return 1;
        }
        for (int t = 0; t < encode_threads; t++) {
            uint32_t row_begin = (uint32_t)(((uint64_t)t * plane_h) / (uint64_t)encode_threads);
            uint32_t row_end = (uint32_t)(((uint64_t)(t + 1) * plane_h) / (uint64_t)encode_threads);
            jobs[t].codec = codec;
            jobs[t].clean = clean;
            jobs[t].qdense = qdense;
            jobs[t].width = width;
            jobs[t].plane_w = plane_w;
            jobs[t].plane_h = plane_h;
            jobs[t].plane_mask = plane_mask;
            jobs[t].sig = sig;
            jobs[t].resid = resid;
            jobs[t].qstep = qstep;
            jobs[t].row_begin = row_begin;
            jobs[t].row_end = row_end;
            if (pthread_create(&threads[t], NULL, encode_worker, &jobs[t]) != 0) {
                fprintf(stderr, "pthread_create failed\n");
                free(codec); free(clean); free(bitmap); free(values);
                free(qdense); free(threads); free(jobs);
                return 1;
            }
        }
        for (int t = 0; t < encode_threads; t++) {
            if (pthread_join(threads[t], NULL) != 0) {
                fprintf(stderr, "pthread_join failed\n");
                free(codec); free(clean); free(bitmap); free(values);
                free(qdense); free(threads); free(jobs);
                return 1;
            }
        }
        for (sample_idx = 0; sample_idx < total_plane_samples; sample_idx++) {
            int16_t q = qdense[sample_idx];
            if (q != 0) {
                bitmap_set(bitmap, sample_idx);
                values[value_count++] = q;
            }
        }
        free(qdense);
        free(threads);
        free(jobs);
    }

    BDRSHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "BDRS", 4);
    h.version = BDRS_VERSION;
    h.width = width;
    h.height = height;
    h.plane_mask = plane_mask;
    h.significant_detail_threshold = sig;
    h.residual_threshold = resid;
    h.quant_step = qstep;
    h.max_value = max_value;
    h.total_plane_samples = total_plane_samples;
    h.bitmap_bytes = bitmap_bytes;
    h.value_count = value_count;
    if (compact && !direct_compact && encode_compact_values(&compact_payload, values, value_count, qstep) != 0) {
        fprintf(stderr, "compact sidecar encoding failed\n");
        free(codec); free(clean); free(bitmap); free(values); free(compact_payload.data);
        return 1;
    }

    FILE *out = fopen(sidecar_path, "wb");
    if (!out) {
        fprintf(stderr, "open %s failed: %s\n", sidecar_path, strerror(errno));
        free(codec); free(clean); free(bitmap); free(values);
        return 1;
    }
    int failed = 0;
    if (compact) {
        BDRCHeader ch;
        memset(&ch, 0, sizeof(ch));
        memcpy(ch.magic, "BDRC", 4);
        ch.version = BDRC_VERSION;
        ch.width = h.width;
        ch.height = h.height;
        ch.plane_mask = h.plane_mask;
        ch.significant_detail_threshold = h.significant_detail_threshold;
        ch.residual_threshold = h.residual_threshold;
        ch.quant_step = h.quant_step;
        ch.max_value = h.max_value;
        ch.total_plane_samples = h.total_plane_samples;
        ch.bitmap_bytes = h.bitmap_bytes;
        ch.value_count = h.value_count;
        ch.value_payload_bytes = (uint64_t)compact_payload.size;
        failed |= fwrite(&ch, 1, sizeof(ch), out) != sizeof(ch);
    } else {
        failed |= fwrite(&h, 1, sizeof(h), out) != sizeof(h);
    }
    failed |= fwrite(bitmap, 1, (size_t)bitmap_bytes, out) != (size_t)bitmap_bytes;
    if (compact) {
        failed |= fwrite(compact_payload.data, 1, compact_payload.size, out) != compact_payload.size;
    } else {
        failed |= fwrite(values, sizeof(int16_t), (size_t)value_count, out) != (size_t)value_count;
    }
    if (fclose(out) != 0) failed = 1;
    if (failed) {
        fprintf(stderr, "write %s failed\n", sidecar_path);
        free(codec); free(clean); free(bitmap); free(values); free(compact_payload.data);
        return 1;
    }
    double elapsed_ms = now_ms() - t0;
    uint64_t value_payload_bytes = compact ? (uint64_t)compact_payload.size : value_count * sizeof(int16_t);
    uint64_t sidecar_bytes =
        (compact ? (uint64_t)sizeof(BDRCHeader) : (uint64_t)sizeof(h)) +
        bitmap_bytes + value_payload_bytes;
    double codec_rmse = rmse_raw(codec, clean, raw_count);

    printf("sidecar_bytes=%llu value_count=%llu nonzero_pct=%.6f elapsed_ms=%.3f codec_clean_rmse=%.6f format=%s\n",
           (unsigned long long)sidecar_bytes, (unsigned long long)value_count,
           total_plane_samples ? 100.0 * (double)value_count / (double)total_plane_samples : 0.0,
           elapsed_ms, codec_rmse, compact ? "compact_varint_qstep" : "bitmap_i16");
    if (receipt_path && write_receipt(receipt_path, "encode", &h, sidecar_bytes, elapsed_ms,
                                      codec_rmse, 0.0, encode_threads,
                                      compact ? "compact_varint_qstep" : "bitmap_i16",
                                      value_payload_bytes)) {
        free(codec); free(clean); free(bitmap); free(values); free(compact_payload.data);
        return 1;
    }

    free(codec);
    free(clean);
    free(bitmap);
    free(values);
    free(compact_payload.data);
    return 0;
}

static int decode_cmd(int argc, char **argv)
{
    if (argc != 7 && argc != 9) {
        fprintf(stderr,
                "usage: %s decode <codec.raw> <sidecar.bdrs> <out.raw> <width> <height> [clean.raw receipt.json]\n",
                argv[0]);
        return 2;
    }
    const char *codec_path = argv[2];
    const char *sidecar_path = argv[3];
    const char *out_path = argv[4];
    const char *clean_path = argc == 9 ? argv[7] : NULL;
    const char *receipt_path = argc == 9 ? argv[8] : NULL;
    uint32_t width = 0, height = 0;
    if (parse_u32(argv[5], &width) || parse_u32(argv[6], &height) || width == 0 || height == 0) {
        fprintf(stderr, "invalid decode dimensions\n");
        return 2;
    }

    uint64_t raw_count = (uint64_t)width * (uint64_t)height;
    size_t raw_bytes = (size_t)raw_count * sizeof(uint16_t);
    uint16_t *codec = (uint16_t *)malloc(raw_bytes);
    uint16_t *out_raw = (uint16_t *)malloc(raw_bytes);
    uint16_t *clean = clean_path ? (uint16_t *)malloc(raw_bytes) : NULL;
    if (!codec || !out_raw || (clean_path && !clean)) {
        fprintf(stderr, "decode allocation failed\n");
        free(codec); free(out_raw); free(clean);
        return 1;
    }
    if (read_file_exact(codec_path, codec, raw_bytes) ||
        (clean_path && read_file_exact(clean_path, clean, raw_bytes))) {
        free(codec); free(out_raw); free(clean);
        return 1;
    }
    memcpy(out_raw, codec, raw_bytes);

    FILE *f = fopen(sidecar_path, "rb");
    if (!f) {
        fprintf(stderr, "open %s failed: %s\n", sidecar_path, strerror(errno));
        free(codec); free(out_raw); free(clean);
        return 1;
    }
    BDRSHeader h;
    memset(&h, 0, sizeof(h));
    char magic[4];
    if (fread(magic, 1, sizeof(magic), f) != sizeof(magic)) {
        fprintf(stderr, "invalid sidecar header\n");
        fclose(f);
        free(codec); free(out_raw); free(clean);
        return 1;
    }
    int compact = 0;
    uint64_t value_payload_bytes = 0;
    if (memcmp(magic, "BDRS", 4) == 0) {
        memcpy(h.magic, magic, 4);
        if (fread((uint8_t *)&h + 4, 1, sizeof(h) - 4, f) != sizeof(h) - 4 ||
            h.version != BDRS_VERSION || h.width != width || h.height != height) {
            fprintf(stderr, "invalid sidecar header\n");
            fclose(f);
            free(codec); free(out_raw); free(clean);
            return 1;
        }
        value_payload_bytes = h.value_count * sizeof(int16_t);
    } else if (memcmp(magic, "BDRC", 4) == 0) {
        BDRCHeader ch;
        memset(&ch, 0, sizeof(ch));
        memcpy(ch.magic, magic, 4);
        if (fread((uint8_t *)&ch + 4, 1, sizeof(ch) - 4, f) != sizeof(ch) - 4 ||
            ch.version != BDRC_VERSION || ch.width != width || ch.height != height ||
            ch.value_payload_bytes == 0) {
            fprintf(stderr, "invalid compact sidecar header\n");
            fclose(f);
            free(codec); free(out_raw); free(clean);
            return 1;
        }
        memcpy(h.magic, "BDRS", 4);
        h.version = BDRS_VERSION;
        h.width = ch.width;
        h.height = ch.height;
        h.plane_mask = ch.plane_mask;
        h.significant_detail_threshold = ch.significant_detail_threshold;
        h.residual_threshold = ch.residual_threshold;
        h.quant_step = ch.quant_step;
        h.max_value = ch.max_value;
        h.total_plane_samples = ch.total_plane_samples;
        h.bitmap_bytes = ch.bitmap_bytes;
        h.value_count = ch.value_count;
        value_payload_bytes = ch.value_payload_bytes;
        compact = 1;
    } else {
        fprintf(stderr, "invalid sidecar magic\n");
        fclose(f);
        free(codec); free(out_raw); free(clean);
        return 1;
    }
    uint8_t *bitmap = (uint8_t *)malloc((size_t)h.bitmap_bytes);
    int16_t *values = (int16_t *)malloc((size_t)h.value_count * sizeof(int16_t));
    uint8_t *compact_payload = compact ? (uint8_t *)malloc((size_t)value_payload_bytes) : NULL;
    if (!bitmap || (!compact && !values) || (compact && !compact_payload)) {
        fprintf(stderr, "sidecar decode allocation failed\n");
        fclose(f);
        free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
        return 1;
    }
    int read_failed = fread(bitmap, 1, (size_t)h.bitmap_bytes, f) != (size_t)h.bitmap_bytes;
    if (compact) {
        read_failed |= fread(compact_payload, 1, (size_t)value_payload_bytes, f) != (size_t)value_payload_bytes;
    } else {
        read_failed |= fread(values, sizeof(int16_t), (size_t)h.value_count, f) != (size_t)h.value_count;
    }
    if (read_failed) {
        fprintf(stderr, "read sidecar payload failed\n");
        fclose(f);
        free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
        return 1;
    }
    fclose(f);

    double t0 = now_ms();
    uint32_t plane_w = width / 2u;
    uint32_t plane_h = height / 2u;
    uint64_t sample_idx = 0;
    uint64_t vi = 0;
    size_t compact_pos = 0;
    for (int plane = 0; plane < 4; plane++) {
        for (uint32_t py = 0; py < plane_h; py++) {
            for (uint32_t px = 0; px < plane_w; px++, sample_idx++) {
                if (!bitmap_get(bitmap, sample_idx)) continue;
                if (vi >= h.value_count) {
                    fprintf(stderr, "sidecar value underflow\n");
                    free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
                    return 1;
                }
                int32_t delta = 0;
                if (compact) {
                    uint32_t encoded = 0;
                    if (varint_get_u32(compact_payload, (size_t)value_payload_bytes,
                                        &compact_pos, &encoded) != 0) {
                        fprintf(stderr, "compact sidecar value decode failed\n");
                        free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
                        return 1;
                    }
                    delta = zigzag_decode_u32(encoded) * h.quant_step;
                } else {
                    delta = (int32_t)values[vi];
                }
                size_t idx = raw_index(width, plane, (int)py, (int)px);
                int32_t v = (int32_t)codec[idx] + delta;
                vi++;
                if (v < 0) v = 0;
                if (v > (int32_t)h.max_value) v = (int32_t)h.max_value;
                out_raw[idx] = (uint16_t)v;
            }
        }
    }
    if (vi != h.value_count || (compact && compact_pos != (size_t)value_payload_bytes)) {
        fprintf(stderr, "sidecar value overflow\n");
        free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
        return 1;
    }
    double elapsed_ms = now_ms() - t0;
    if (write_file_exact(out_path, out_raw, raw_bytes)) {
        free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
        return 1;
    }
    double codec_rmse = clean ? rmse_raw(codec, clean, raw_count) : 0.0;
    double output_rmse = clean ? rmse_raw(out_raw, clean, raw_count) : 0.0;
    uint64_t sidecar_bytes =
        (compact ? (uint64_t)sizeof(BDRCHeader) : (uint64_t)sizeof(h)) +
        h.bitmap_bytes + value_payload_bytes;
    printf("decoded sidecar_bytes=%llu value_count=%llu elapsed_ms=%.3f codec_clean_rmse=%.6f output_clean_rmse=%.6f format=%s\n",
           (unsigned long long)sidecar_bytes, (unsigned long long)h.value_count,
           elapsed_ms, codec_rmse, output_rmse, compact ? "compact_varint_qstep" : "bitmap_i16");
    if (receipt_path && write_receipt(receipt_path, "decode", &h, sidecar_bytes, elapsed_ms,
                                      codec_rmse, output_rmse, 0,
                                      compact ? "compact_varint_qstep" : "bitmap_i16",
                                      value_payload_bytes)) {
        free(codec); free(out_raw); free(clean); free(bitmap); free(values); free(compact_payload);
        return 1;
    }

    free(codec);
    free(out_raw);
    free(clean);
    free(bitmap);
    free(values);
    free(compact_payload);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s encode|decode ...\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "encode") == 0) return encode_cmd(argc, argv);
    if (strcmp(argv[1], "decode") == 0) return decode_cmd(argc, argv);
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 2;
}
