/* .gvid -> Mission 1 1024x768 RGB24 preview frames.
   Usage:
     gvid_preview_rgb_cli in.gvid SENSOR_W SENSOR_H out_dir limit write_frames

   write_frames=1 writes frame_%06d.rgb. write_frames=0 measures only.
   limit=0 uses the frame count hint, or reads to EOF if the hint is unknown.
   A positive limit requires exactly that many complete frames.
*/
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "gpr_video_format.h"

/* Operational limits, not format limits. All size products fit in size_t
   on the supported 32-bit and 64-bit targets. */
#define MAX_SENSOR_DIM 16384
#define MAX_SENSOR_PIXELS (128u * 1024u * 1024u)
#define MAX_PAYLOAD (128u * 1024u * 1024u)
#define MAX_FRAMES 1000000
#define FUSED_WIRE_HEADER_SIZE 48

extern int gpr_decode_fused_ll_preview(const uint8_t *enc, size_t enc_size,
                                       uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                       int *out_width, int *out_height);

typedef struct {
    double decode_ms;
    double isp_ms;
    double write_ms;
    double total_ms;
    uint32_t payload_size;
} Row;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int parse_int(const char *s, int min, int max, int *out) {
    if (!*s) return -1;
    for (const char *p = s; *p; p++)
        if (*p < '0' || *p > '9') return -1;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < min || v > max) return -1;
    *out = (int)v;
    return 0;
}

static int make_dir(const char *path) {
    if (mkdir(path, 0775) == 0) return 0;
    struct stat st;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

static int valid_payload_header(const uint8_t *p, const gpr_video_clip_header *clip) {
    /* FUSD v1 is twelve LE uint32s. Do not depend on the decoder's internal
       FUSED_HEADER struct or let it derive write dimensions from unchecked data. */
    uint32_t pf = rd32(p + 16), prescale = rd32(p + 32);
    return rd32(p) == 0x44535546u && rd32(p + 4) == 1 &&
           rd32(p + 8) == clip->width && rd32(p + 12) == clip->height &&
           pf == clip->pixel_format && rd32(p + 20) <= GPR_VIDEO_QUALITY_MAX &&
           rd32(p + 24) == 1 && rd32(p + 28) == (pf == 4 ? 16u : 14u) &&
           (prescale == 0 || prescale == 2 || prescale == 3) &&
           rd32(p + 36) == 0 && rd32(p + 40) == 16 && rd32(p + 44) <= 1;
}

static void json_string(FILE *out, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', out);
    while (*p) {
        unsigned c = *p++;
        if (c == '"' || c == '\\') {
            fputc('\\', out);
            fputc((int)c, out);
        } else if (c < 32) {
            fprintf(out, "\\u%04x", c);
        } else if (c < 128) {
            fputc((int)c, out);
        } else {
            /* Preserve UTF-8 paths; escape invalid filesystem bytes as Latin-1
               code points so even non-UTF-8 POSIX names produce valid JSON. */
            int n = c >= 0xc2 && c <= 0xdf ? 1 :
                    c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
            unsigned cp = c & (n == 1 ? 31u : n == 2 ? 15u : 7u);
            int i = 0;
            for (; i < n && p[i] >= 0x80 && p[i] <= 0xbf; i++)
                cp = (cp << 6) | (p[i] & 63u);
            if (n && i == n && cp >= (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) &&
                cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) {
                fputc((int)c, out);
                fwrite(p, 1, (size_t)n, out);
                p += n;
            } else {
                fprintf(out, "\\u%04x", c);
            }
        }
    }
    fputc('"', out);
}

static int percentile_u16_hist(const uint32_t hist[16384], uint32_t total, double frac) {
    uint32_t want = (uint32_t)((double)(total - 1) * frac + 0.5);
    uint32_t seen = 0;
    for (int i = 0; i < 16384; i++) {
        seen += hist[i];
        if (seen > want) return i;
    }
    return 16383;
}

static uint8_t clamp_u8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

static int preview_isp_rgb24(const uint16_t *raw, int width, int height,
                             int pitch_pixels, uint8_t *rgb_out) {
    double r_sum = 0.0, g_sum = 0.0, b_sum = 0.0;
    uint32_t blocks = 0;
    for (int y = 0; y < height; y += 2) {
        const uint16_t *row0 = raw + (size_t)y * pitch_pixels;
        const uint16_t *row1 = row0 + pitch_pixels;
        for (int x = 0; x < width; x += 2) {
            r_sum += row0[x];
            g_sum += ((double)row0[x + 1] + (double)row1[x]) * 0.5;
            b_sum += row1[x + 1];
            blocks++;
        }
    }
    float r_gain = (float)((g_sum / blocks) / fmax(r_sum / blocks, 1.0));
    float b_gain = (float)((g_sum / blocks) / fmax(b_sum / blocks, 1.0));
    if (r_gain < 0.25f) r_gain = 0.25f;
    if (r_gain > 4.0f) r_gain = 4.0f;
    if (b_gain < 0.25f) b_gain = 0.25f;
    if (b_gain > 4.0f) b_gain = 4.0f;

    uint32_t hist[16384];
    memset(hist, 0, sizeof(hist));
    for (int y = 0; y < height; y += 2) {
        const uint16_t *row0 = raw + (size_t)y * pitch_pixels;
        const uint16_t *row1 = row0 + pitch_pixels;
        for (int x = 0; x < width; x += 2) {
            float r = row0[x] * r_gain;
            float g = ((float)row0[x + 1] + (float)row1[x]) * 0.5f;
            float b = row1[x + 1] * b_gain;
            int lum = (int)(0.2126f * r + 0.7152f * g + 0.0722f * b + 0.5f);
            if (lum < 0) lum = 0;
            if (lum > 16383) lum = 16383;
            hist[lum]++;
        }
    }

    int lo = percentile_u16_hist(hist, blocks, 0.008);
    int hi = percentile_u16_hist(hist, blocks, 0.998);
    if (hi <= lo + 64) {
        lo = 0;
        hi = 16383;
    }
    uint8_t lut[16384];
    float inv = 1.0f / (float)(hi - lo);
    for (int i = 0; i < 16384; i++) {
        float x = ((float)i - (float)lo) * inv;
        if (x < 0.0f) x = 0.0f;
        if (x > 1.0f) x = 1.0f;
        x = powf(x, 1.0f / 2.25f);
        float xp = powf(x, 1.18f);
        float yp = powf(1.0f - x, 1.18f);
        x = xp / (xp + yp + 1.0e-9f);
        lut[i] = clamp_u8(x * 255.0f);
    }

    for (int y = 0; y < height; y += 2) {
        const uint16_t *row0 = raw + (size_t)y * pitch_pixels;
        const uint16_t *row1 = row0 + pitch_pixels;
        uint8_t *out0 = rgb_out + (size_t)y * width * 3;
        uint8_t *out1 = out0 + (size_t)width * 3;
        for (int x = 0; x < width; x += 2) {
            int rv = (int)(row0[x] * r_gain + 0.5f);
            int gv = (int)(((float)row0[x + 1] + (float)row1[x]) * 0.5f + 0.5f);
            int bv = (int)(row1[x + 1] * b_gain + 0.5f);
            if (rv < 0) rv = 0; if (rv > 16383) rv = 16383;
            if (gv < 0) gv = 0; if (gv > 16383) gv = 16383;
            if (bv < 0) bv = 0; if (bv > 16383) bv = 16383;
            float r = lut[rv], g = lut[gv], b = lut[bv];
            float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            r = lum + (r - lum) * 1.12f;
            g = lum + (g - lum) * 1.12f;
            b = lum + (b - lum) * 1.12f;
            uint8_t r8 = clamp_u8(r), g8 = clamp_u8(g), b8 = clamp_u8(b);
            size_t p = (size_t)x * 3;
            out0[p + 0] = r8; out0[p + 1] = g8; out0[p + 2] = b8;
            out0[p + 3] = r8; out0[p + 4] = g8; out0[p + 5] = b8;
            out1[p + 0] = r8; out1[p + 1] = g8; out1[p + 2] = b8;
            out1[p + 3] = r8; out1[p + 4] = g8; out1[p + 5] = b8;
        }
    }
    return 0;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static double median_of(const Row *rows, int n, int field) {
    double *vals = (double *)malloc((size_t)n * sizeof(double));
    if (!vals) return NAN;
    for (int i = 0; i < n; i++) {
        vals[i] = field == 0 ? rows[i].decode_ms : field == 1 ? rows[i].isp_ms :
                  field == 2 ? rows[i].write_ms : rows[i].total_ms;
    }
    qsort(vals, (size_t)n, sizeof(double), cmp_double);
    double med = (n & 1) ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) * 0.5;
    free(vals);
    return med;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s in.gvid SENSOR_W SENSOR_H out_dir limit write_frames\n", argv[0]);
        return 1;
    }
    const char *gvid_path = argv[1];
    int sensor_w, sensor_h, limit, write_frames;
    const char *out_dir = argv[4];
    if (parse_int(argv[2], 8, MAX_SENSOR_DIM, &sensor_w) ||
        parse_int(argv[3], 8, MAX_SENSOR_DIM, &sensor_h) ||
        parse_int(argv[5], 0, MAX_FRAMES, &limit) ||
        parse_int(argv[6], 0, 1, &write_frames) ||
        sensor_w % 8 || sensor_h % 8 ||
        (size_t)sensor_w * sensor_h > MAX_SENSOR_PIXELS) {
        fprintf(stderr, "invalid dimensions, limit, or write_frames\n");
        return 1;
    }
    int preview_w = sensor_w / 4;
    int preview_h = sensor_h / 4;

    char frames_dir[1024];
    char receipt_path[1024];
    int nf = snprintf(frames_dir, sizeof(frames_dir), "%s/frames_rgb", out_dir);
    int nr = snprintf(receipt_path, sizeof(receipt_path), "%s/receipt.json", out_dir);
    if (nf < 0 || (size_t)nf >= sizeof(frames_dir) ||
        nr < 0 || (size_t)nr >= sizeof(receipt_path)) {
        fprintf(stderr, "output path too long\n");
        return 1;
    }

    FILE *in = fopen(gvid_path, "rb");
    if (!in) { perror("open gvid"); return 2; }
    uint8_t hdr[GPR_VIDEO_CLIP_HEADER_SIZE];
    gpr_video_clip_header clip;
    if (fread(hdr, 1, sizeof(hdr), in) != sizeof(hdr) ||
        gpr_video_read_clip_header(hdr, sizeof(hdr), &clip) != 0 ||
        clip.width != (uint32_t)sensor_w || clip.height != (uint32_t)sensor_h ||
        !(clip.pixel_format == 0 || clip.pixel_format == 1 || clip.pixel_format == 4)) {
        fprintf(stderr, "bad or unsupported gvid header/dimensions (RGGB required)\n");
        fclose(in);
        return 3;
    }
    if (limit == 0 && clip.frame_count_hint > MAX_FRAMES) {
        fprintf(stderr, "frame hint exceeds preview limit\n");
        fclose(in);
        return 3;
    }
    int required = limit ? limit : (int)clip.frame_count_hint;
    int whole_clip = limit == 0 || (clip.frame_count_hint && (uint32_t)limit >= clip.frame_count_hint);
    limit = required ? required : MAX_FRAMES;
    int status = 7, count = 0;
    uint16_t *bayer = NULL;
    uint8_t *rgb = NULL;
    uint8_t *payload = NULL;
    size_t payload_cap = 0;
    Row *rows = NULL;
    size_t rows_cap = 0;
    uint64_t previous_tag = 0;
    size_t pixels = (size_t)preview_w * preview_h;
    if (make_dir(out_dir) || (write_frames && make_dir(frames_dir))) {
        perror("create output directory");
        goto cleanup;
    }

    for (; count < limit; count++) {
        uint8_t fh[GPR_VIDEO_FRAME_HEADER_SIZE], fused[FUSED_WIRE_HEADER_SIZE];
        gpr_video_frame_header frame;
        size_t got = fread(fh, 1, sizeof(fh), in);
        if (got == 0 && feof(in) && !ferror(in) && !required) break;
        if (got != sizeof(fh) || gpr_video_read_frame_header(fh, sizeof(fh), &frame)) {
            fprintf(stderr, "bad or truncated frame header at frame=%d\n", count);
            goto cleanup;
        }
        uint32_t payload_size = frame.payload_size;
        if (count && frame.frame_tag <= previous_tag) {
            fprintf(stderr, "nonmonotonic frame tag\n");
            goto cleanup;
        }
        previous_tag = frame.frame_tag;
        if (payload_size < sizeof(fused) + 16 * 4 || payload_size > MAX_PAYLOAD ||
            fread(fused, 1, sizeof(fused), in) != sizeof(fused) ||
            !valid_payload_header(fused, &clip)) {
            fprintf(stderr, "bad or unsupported FUSD header/size at frame=%d\n", count);
            goto cleanup;
        }
        if (!bayer) {
            bayer = (uint16_t *)malloc(pixels * sizeof(uint16_t));
            rgb = (uint8_t *)malloc(pixels * 3);
            if (!bayer || !rgb) goto cleanup;
        }
        if ((size_t)count == rows_cap) {
            size_t cap = rows_cap ? rows_cap * 2 : 64;
            if (cap > (size_t)limit) cap = (size_t)limit;
            Row *next = (Row *)realloc(rows, cap * sizeof(Row));
            if (!next) goto cleanup;
            rows = next;
            rows_cap = cap;
        }
        if (payload_size > payload_cap) {
            uint8_t *np = (uint8_t *)realloc(payload, payload_size);
            if (!np) goto cleanup;
            payload = np;
            payload_cap = payload_size;
        }
        memcpy(payload, fused, sizeof(fused));
        size_t remaining = payload_size - sizeof(fused);
        if (fread(payload + sizeof(fused), 1, remaining, in) != remaining) {
            fprintf(stderr, "truncated payload at frame=%d\n", count);
            goto cleanup;
        }
        /* Check with subtraction: the decoder's off + band_size check can
           wrap on 32-bit targets. The wire header above fixes the band count. */
        size_t band_remaining = payload_size - sizeof(fused) - 16 * 4;
        for (int band = 0; band < 16; band++) {
            uint32_t size = rd32(payload + sizeof(fused) + (size_t)band * 4);
            if (size > band_remaining) {
                fprintf(stderr, "invalid band size at frame=%d\n", count);
                goto cleanup;
            }
            band_remaining -= size;
        }
        if (band_remaining != 0) {
            fprintf(stderr, "unexpected trailing payload bytes at frame=%d\n", count);
            goto cleanup;
        }

        double t0 = now_ms();
        int ow = 0, oh = 0;
        int rc = gpr_decode_fused_ll_preview(payload, payload_size, bayer,
                                             (size_t)preview_w * sizeof(uint16_t), &ow, &oh);
        double t1 = now_ms();
        if (rc != 0 || ow != preview_w || oh != preview_h) {
            fprintf(stderr, "decode failed frame=%d rc=%d dims=%dx%d\n", count, rc, ow, oh);
            goto cleanup;
        }
        rc = preview_isp_rgb24(bayer, preview_w, preview_h, preview_w, rgb);
        double t2 = now_ms();
        if (rc != 0) goto cleanup;
        if (write_frames) {
            char path[1200];
            int n = snprintf(path, sizeof(path), "%s/frame_%06d.rgb", frames_dir, count);
            if (n < 0 || (size_t)n >= sizeof(path)) goto cleanup;
            FILE *out = fopen(path, "wb");
            if (!out) { perror("open RGB output"); goto cleanup; }
            int failed = fwrite(rgb, 1, pixels * 3, out) != pixels * 3;
            if (fclose(out) != 0) failed = 1;
            if (failed) {
                fprintf(stderr, "RGB write failed at frame=%d\n", count);
                goto cleanup;
            }
        }
        double t3 = now_ms();
        rows[count].decode_ms = t1 - t0;
        rows[count].isp_ms = t2 - t1;
        rows[count].write_ms = t3 - t2;
        rows[count].total_ms = t3 - t0;
        rows[count].payload_size = payload_size;
        if ((count + 1) % 50 == 0) fprintf(stderr, "%d frames\n", count + 1);
    }
    if (!count || (!required && count == MAX_FRAMES)) {
        fprintf(stderr, "empty stream or frame limit exceeded\n");
        goto cleanup;
    }
    if (whole_clip && (fgetc(in) != EOF || ferror(in))) {
        fprintf(stderr, "unexpected trailing stream data\n");
        goto cleanup;
    }

    {
        double dec_med = median_of(rows, count, 0);
        double isp_med = median_of(rows, count, 1);
        double wr_med = median_of(rows, count, 2);
        double total_med = median_of(rows, count, 3);
        if (!isfinite(dec_med) || !isfinite(isp_med) || !isfinite(wr_med) ||
            !isfinite(total_med)) goto cleanup;
        FILE *receipt = fopen(receipt_path, "w");
        if (!receipt) { perror("open receipt"); goto cleanup; }
        fputs("{\n  \"schema\": \"gvid_preview_rgb_cli.v1\",\n  \"gvid\": ", receipt);
        json_string(receipt, gvid_path);
        fprintf(receipt,
                ",\n"
                "  \"frames\": %d,\n"
                "  \"width\": %d,\n"
                "  \"height\": %d,\n"
                "  \"write_frames\": %d,\n"
                "  \"summary\": {\n"
                "    \"decode_ms_median\": %.3f,\n"
                "    \"isp_ms_median\": %.3f,\n"
                "    \"write_ms_median\": %.3f,\n"
                "    \"total_ms_median\": %.3f,\n"
                "    \"total_fps_median\": %.3f\n"
                "  }\n"
                "}\n",
                count, preview_w, preview_h, write_frames,
                dec_med, isp_med, wr_med, total_med, total_med > 0.0 ? 1000.0 / total_med : 0.0);
        int failed = ferror(receipt);
        if (fclose(receipt) != 0) failed = 1;
        if (failed) { fprintf(stderr, "receipt write failed\n"); goto cleanup; }
        fprintf(stderr, "receipt %s\n", receipt_path);
    }
    status = 0;

cleanup:
    if (fclose(in) != 0) status = 7;
    free(rows);
    free(payload);
    free(rgb);
    free(bayer);
    if (status) fprintf(stderr, "preview failed after %d complete frames\n", count);
    return status;
}
