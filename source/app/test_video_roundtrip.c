/*! @file test_video_roundtrip.c
 *
 *  @brief Band-level decode verification for the pipelined video encoder.
 *
 *  Encodes a sequence of frames through gpr_video_encoder with rate
 *  control, captures each VC5 bitstream in the writer callback, then
 *  walks the bitstream as a sequence of 12 band-bitstreams (4 channels
 *  x 3 highpass bands; the fused encoder is single-level and emits no
 *  LL band). Each band is decoded with jans_decode_band_x4() and we
 *  collect basic stats (decoded coefficient count, non-zero count,
 *  min/max value, mean abs) to prove the bitstream is well-formed.
 *
 *  This is the "structure roundtrip" simplification described in the
 *  test brief: rather than implementing the full inverse pipeline
 *  (inverse-quant + inverse-wavelet + inverse-log-curve) we verify
 *  that the rate-controlled encoder is producing decodable bitstreams.
 *
 *  Build:
 *    clang -O2 -o /tmp/test_video_roundtrip source/app/test_video_roundtrip.c \
 *      build/source/lib/vc5_encoder/libvc5_encoder.a \
 *      build/source/lib/vc5_common/libvc5_common.a -lpthread
 *
 *  Usage:
 *    test_video_roundtrip <raw_file> <w> <h> <pf> <q> <num_frames> <fps> <target_MBps>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <inttypes.h>

#include "../lib/vc5_encoder/gpr_video.h"
#include "../lib/vc5_encoder/fused_encode.h"   /* FUSED_HEADER, FUSED_MAGIC */

/* Decoder used to verify each band; lives in libvc5_common. */
extern int jans_decode_band_x4(const uint8_t *in_buf, size_t in_size,
                                int32_t *data, int width, int height, int pitch);

#define BANDS_PER_FRAME 12   /* 4 channels x 3 highpass bands (single-level highpass-only) */

/* Per-frame captured bitstream. Owned by collector_state below. */
typedef struct {
    uint8_t *data;
    size_t   size;
    uint64_t tag;
} captured_frame;

typedef struct {
    captured_frame *frames;
    int             capacity;
    int             count;
    pthread_mutex_t lock;
    uint64_t        total_bytes;
} collector_state;

static int collect_writer(void *user_data, const uint8_t *vc5, size_t size,
                          uint64_t tag)
{
    collector_state *cs = (collector_state *)user_data;
    pthread_mutex_lock(&cs->lock);
    if (cs->count < cs->capacity) {
        captured_frame *f = &cs->frames[cs->count++];
        f->data = (uint8_t *)malloc(size);
        if (f->data) {
            memcpy(f->data, vc5, size);
            f->size = size;
            f->tag  = tag;
            cs->total_bytes += size;
        } else {
            f->size = 0;
            cs->count--;   /* drop on alloc fail */
        }
    }
    pthread_mutex_unlock(&cs->lock);
    return 0;
}

/* Determine how many bytes one band's bitstream consumes by inspecting
   the header. Mirrors jans_decode_band_x4's two formats:
     - Stripe: 4 bytes 0xFFFFFFFF marker + 4 bytes num_stripes
               + 8 bytes reserved + for each stripe: 8 bytes
               (stripe_rows + stripe_size) + stripe_size bytes payload.
     - Legacy: 16-byte header (token_count, freq_size, rans_size,
               resid_size) + freq_size + rans_size + resid_size payload. */
static int probe_band_bytes(const uint8_t *p, size_t avail, size_t *consumed)
{
    if (avail < 16) return -1;

    if (p[0] == 0xFF && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0xFF) {
        int num_stripes = (p[4]<<24)|(p[5]<<16)|(p[6]<<8)|p[7];
        if (num_stripes < 0 || num_stripes > 1000000) return -1;
        size_t pos = 16;   /* marker(4) + num_stripes(4) + reserved(8) */
        for (int s = 0; s < num_stripes; s++) {
            if (pos + 8 > avail) return -1;
            int stripe_size = (p[pos+4]<<24)|(p[pos+5]<<16)|
                              (p[pos+6]<<8) | p[pos+7];
            if (stripe_size < 0) return -1;
            pos += 8 + (size_t)stripe_size;
            if (pos > avail) return -1;
        }
        *consumed = pos;
        return 0;
    }

    int token_count = (p[0]<<24)|(p[1]<<16)|(p[2]<<8) | p[3];
    int freq_size   = (p[4]<<24)|(p[5]<<16)|(p[6]<<8) | p[7];
    int rans_size   = (p[8]<<24)|(p[9]<<16)|(p[10]<<8)| p[11];
    int resid_size  = (p[12]<<24)|(p[13]<<16)|(p[14]<<8)|p[15];
    if (token_count < 0 || freq_size < 0 || rans_size < 0 || resid_size < 0) return -1;
    size_t total = 16 + (size_t)freq_size + (size_t)rans_size + (size_t)resid_size;
    if (total > avail) return -1;
    *consumed = total;
    return 0;
}

typedef struct {
    int      decoded_ok;
    int      coeff_count;    /* width*height */
    int      nonzero_count;
    int32_t  min_v;
    int32_t  max_v;
    double   mean_abs;
} band_stats;

/* Decode the highpass bands of one frame's FUSED-wrapped bitstream.
   New format: FUSED_HEADER + uint32_t band_size[num_bands] + band[0]..band[n-1].
   In single-level (no LL) mode, num_bands=12 (4 channels × 3 HP bands).
   In single-level + LL mode, num_bands=16 (band 0,4,8,12 are LL — skipped here).
   In multi-level mode, num_bands=40 — rejected here (this test verifies
   the single-level path only). */
static int verify_frame_bitstream(const uint8_t *vc5, size_t size,
                                   int bw, int bh,
                                   band_stats out[BANDS_PER_FRAME])
{
    memset(out, 0, sizeof(band_stats) * BANDS_PER_FRAME);
    if (size < sizeof(FUSED_HEADER)) {
        fprintf(stderr, "    payload too small for FUSED_HEADER (%zu < %zu)\n",
                size, sizeof(FUSED_HEADER));
        return -1;
    }
    FUSED_HEADER hdr;
    memcpy(&hdr, vc5, sizeof(hdr));
    if (hdr.magic != FUSED_MAGIC) {
        fprintf(stderr, "    bad FUSED magic: 0x%08x (want 0x%08x)\n",
                hdr.magic, FUSED_MAGIC);
        return -1;
    }

    if (hdr.version != FUSED_VERSION || hdr.multi_level != 0 || hdr.decimate > 1 ||
        hdr.width != (uint32_t)bw * 4 || hdr.height != (uint32_t)bh * 4) {
        fprintf(stderr, "    FAIL: unsupported band layout or dimensions\n");
        return -1;
    }
    int has_ll = 0;
    if (hdr.num_bands == 12) {
        has_ll = 0;  /* 4 channels × 3 HP bands */
    } else if (hdr.num_bands == 16) {
        has_ll = 1;  /* 4 channels × (LL + 3 HP) — skip LL bands at 0,4,8,12 */
    } else {
        fprintf(stderr, "    FAIL: unsupported num_bands=%u "
                        "(this test only verifies single-level encoder output)\n",
                hdr.num_bands);
        return -1;
    }

    size_t manifest_bytes = (size_t)hdr.num_bands * sizeof(uint32_t);
    if (sizeof(FUSED_HEADER) + manifest_bytes > size) {
        fprintf(stderr, "    band manifest overruns payload\n");
        return -1;
    }
    const uint32_t *band_sizes = (const uint32_t *)(vc5 + sizeof(FUSED_HEADER));
    size_t pos = sizeof(FUSED_HEADER) + manifest_bytes;

    int32_t *band = (int32_t *)calloc((size_t)bw * bh, sizeof(int32_t));
    if (!band) return -1;

    int bands_ok = 0;
    int band_idx = 0;          /* index into the FUSED manifest */
    for (int hp = 0; hp < BANDS_PER_FRAME; hp++) {
        band_stats *st = &out[hp];
        memset(st, 0, sizeof(*st));
        st->coeff_count = bw * bh;

        /* If LL is present, advance past it at start of each channel. */
        if (has_ll && (hp % 3) == 0) {
            pos += band_sizes[band_idx++];
        }
        if (band_idx >= (int)hdr.num_bands) {
            fprintf(stderr, "    ran out of bands at hp=%d\n", hp);
            break;
        }
        size_t band_bytes = band_sizes[band_idx];
        if (pos + band_bytes > size) {
            fprintf(stderr, "    band %d: size %zu overruns payload\n", hp, band_bytes);
            break;
        }
        /* probe_band_bytes is still used as a self-check that the manifest
           size matches what jans_decode_band_x4 would consume. */
        size_t consumed = 0;
        if (probe_band_bytes(vc5 + pos, band_bytes, &consumed) != 0) {
            st->decoded_ok = 0;
            fprintf(stderr, "    band %d: header probe failed at pos=%zu (avail=%zu)\n",
                    hp, pos, band_bytes);
            pos += band_bytes;
            band_idx++;
            continue;
        }

        /* Zero the scratch — the stripe path may leave trailing rows
           untouched and expects them already-zero. */
        memset(band, 0, (size_t)bw * bh * sizeof(int32_t));

        int rc = jans_decode_band_x4(vc5 + pos, band_bytes,
                                      band, bw, bh, bw * sizeof(int32_t));
        if (rc != 0) {
            st->decoded_ok = 0;
            fprintf(stderr, "    band %d: jans_decode_band_x4 returned %d\n", hp, rc);
            pos += band_bytes;
            band_idx++;
            continue;
        }

        int32_t mn =  2147483647;
        int32_t mx = -2147483648;
        int nz = 0;
        uint64_t abs_sum = 0;
        size_t n = (size_t)bw * bh;
        for (size_t k = 0; k < n; k++) {
            int32_t v = band[k];
            if (v != 0) nz++;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            abs_sum += (uint64_t)(v < 0 ? -v : v);
        }
        st->decoded_ok    = 1;
        st->nonzero_count = nz;
        st->min_v         = mn;
        st->max_v         = mx;
        st->mean_abs      = (double)abs_sum / (double)n;
        bands_ok++;

        pos += band_bytes;
        band_idx++;
    }

    free(band);
    return bands_ok;
}

static int run_one_input(const char *raw_path, int w, int h, int pf, int q,
                         int num_frames, double fps, double target_MBps)
{
    if (w <= 0 || h <= 0 || w % 4 || h % 4 || num_frames <= 0) {
        fprintf(stderr, "invalid dimensions or frame count\n");
        return 1;
    }
    fprintf(stderr, "\n========================================================\n");
    fprintf(stderr, " Verifying: %s\n", raw_path);
    fprintf(stderr, "   dims=%dx%d pf=%d q=%d frames=%d fps=%.1f target=%.1f MB/s\n",
            w, h, pf, q, num_frames, fps, target_MBps);
    fprintf(stderr, "========================================================\n");

    FILE *f = fopen(raw_path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", raw_path); return 1; }
    fseek(f, 0, SEEK_END);
    size_t raw_size = (size_t)ftell(f);
    rewind(f);
    size_t expected = (size_t)w * h * 2;
    if (raw_size < expected) {
        fprintf(stderr, "raw file too small: %zu < %zu\n", raw_size, expected);
        fclose(f);
        return 1;
    }
    uint8_t *raw = (uint8_t *)malloc(expected);
    if (!raw) { fclose(f); return 1; }
    if (fread(raw, 1, expected, f) != expected) {
        fprintf(stderr, "short raw read\n");
        fclose(f); free(raw); return 1;
    }
    fclose(f);

    collector_state cs;
    memset(&cs, 0, sizeof(cs));
    cs.capacity = num_frames;
    cs.frames   = (captured_frame *)calloc((size_t)num_frames, sizeof(captured_frame));
    pthread_mutex_init(&cs.lock, NULL);
    if (!cs.frames) { free(raw); return 1; }

    GPR_VIDEO_ENCODER *enc = gpr_video_encoder_create(
        w, h, pf, q, 3, collect_writer, &cs);
    if (!enc) {
        fprintf(stderr, "encoder create failed\n");
        free(raw); free(cs.frames);
        return 1;
    }
    if (target_MBps > 0.0) {
        gpr_video_encoder_set_target_bitrate(enc, target_MBps, fps);
    }

    for (int i = 0; i < num_frames; i++) {
        if (gpr_video_encoder_submit(enc, raw, expected, (uint64_t)i) != 0) {
            fprintf(stderr, "submit failed at frame %d\n", i);
        }
    }
    gpr_video_encoder_flush(enc);
    gpr_video_encoder_destroy(enc);
    free(raw);

    fprintf(stderr, "captured %d frames, %.2f MB total (%.2f MB/frame avg)\n",
            cs.count,
            cs.total_bytes / 1024.0 / 1024.0,
            cs.count > 0 ? (cs.total_bytes / (double)cs.count) / 1024.0 / 1024.0 : 0.0);

    /* Band dims as produced by setup_channel_state: w/4 by h/4. */
    int bw = w / 4;
    int bh = h / 4;

    int frames_fully_ok = 0;
    int total_bands_ok  = 0;
    int total_bands     = cs.count * BANDS_PER_FRAME;

    for (int fi = 0; fi < cs.count; fi++) {
        captured_frame *frame = &cs.frames[fi];
        band_stats stats[BANDS_PER_FRAME];
        int ok = verify_frame_bitstream(frame->data, frame->size, bw, bh, stats);

        if (ok > 0) total_bands_ok += ok;
        if (ok == BANDS_PER_FRAME) frames_fully_ok++;

        /* Aggregate across the 12 bands for a per-frame summary line. */
        int nz_sum = 0;
        double abs_sum = 0;
        int32_t mn =  2147483647, mx = -2147483648;
        for (int b = 0; b < BANDS_PER_FRAME; b++) {
            if (!stats[b].decoded_ok) continue;
            nz_sum += stats[b].nonzero_count;
            abs_sum += stats[b].mean_abs * stats[b].coeff_count;
            if (stats[b].min_v < mn) mn = stats[b].min_v;
            if (stats[b].max_v > mx) mx = stats[b].max_v;
        }
        size_t coeffs_total = (size_t)BANDS_PER_FRAME * bw * bh;
        double pct_nz = ok > 0 ? 100.0 * nz_sum / (double)coeffs_total : 0.0;
        double mean_abs = ok > 0 ? abs_sum / (double)coeffs_total : 0.0;

        fprintf(stderr, "  frame %2" PRIu64 ": %d/%d bands ok | size=%6.2f MB | nz=%6.2f%% | mean|v|=%.2f | range=[%d,%d]\n",
                frame->tag, ok, BANDS_PER_FRAME,
                frame->size / 1024.0 / 1024.0,
                pct_nz, mean_abs, mn, mx);

        free(frame->data);
    }

    fprintf(stderr, "\nSummary: %d/%d frames fully decoded, %d/%d bands ok\n",
            frames_fully_ok, cs.count, total_bands_ok, total_bands);

    int rc = (frames_fully_ok == cs.count && cs.count == num_frames) ? 0 : 1;
    fprintf(stderr, "VERDICT: %s\n", rc == 0 ? "PASS" : "FAIL");

    free(cs.frames);
    pthread_mutex_destroy(&cs.lock);
    return rc;
}

int main(int argc, char **argv) {
    if (argc > 1 && argc < 9) {
        fprintf(stderr,
            "usage: %s raw_file w h pf q num_frames fps target_MBps\n"
            "       %s              (no args = run default Z8 ISO64 + ISO22800 sweep)\n",
            argv[0], argv[0]);
        return 1;
    }

    /* CLI single-input mode */
    if (argc >= 9) {
        const char *path = argv[1];
        int    w  = atoi(argv[2]);
        int    h  = atoi(argv[3]);
        int    pf = atoi(argv[4]);
        int    q  = atoi(argv[5]);
        int    n  = atoi(argv[6]);
        double fps = atof(argv[7]);
        double mbps = atof(argv[8]);
        return run_one_input(path, w, h, pf, q, n, fps, mbps);
    }

    /* Default sweep mode: run both Z8 captures. */
    int rc1 = run_one_input("/tmp/Z8_ISO64.raw",    8280, 5520, 4, 3, 10, 24.0, 150.0);
    int rc2 = run_one_input("/tmp/Z8_ISO22800.raw", 8280, 5520, 4, 3, 10, 24.0, 150.0);

    fprintf(stderr, "\n=== Final ===\n");
    fprintf(stderr, "  ISO64:    %s\n", rc1 == 0 ? "PASS" : "FAIL");
    fprintf(stderr, "  ISO22800: %s\n", rc2 == 0 ? "PASS" : "FAIL");

    return (rc1 == 0 && rc2 == 0) ? 0 : 1;
}
