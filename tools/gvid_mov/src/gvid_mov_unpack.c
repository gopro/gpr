/* gvid_mov_unpack — single .mov GVIDMov container → directory of .gpr files.
 *
 * Usage:
 *   gvid_mov_unpack INPUT.mov OUTPUT_DIR
 *
 * Produces frame_0000.gpr, frame_0001.gpr, ... — byte-identical to the
 * frames the writer was handed. Mostly for debugging and round-trip
 * verification; the playback pipeline reads via gvid_mov_reader_* directly.
 */

#define _POSIX_C_SOURCE 200809L

#include "gvid_mov.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <libavutil/error.h>

static int ensure_dir(const char *p) {
    struct stat st;
    if (stat(p, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a directory\n", p);
            return -1;
        }
        return 0;
    }
    if (mkdir(p, 0755) == 0) return 0;
    fprintf(stderr, "mkdir %s: %s\n", p, strerror(errno));
    return -1;
}

static int write_file(const char *path, const uint8_t *buf, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
    size_t wr = fwrite(buf, 1, n, f);
    int close_rc = fclose(f);
    if (wr != n || close_rc != 0) { fprintf(stderr, "write failed: %s\n", path); return -1; }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s INPUT.mov OUTPUT_DIR\n", argv0);
}

int main(int argc, char **argv) {
    if (argc != 3) { usage(argv[0]); return 2; }
    const char *in_path = argv[1];
    const char *out_dir = argv[2];

    if (ensure_dir(out_dir) != 0) return 1;

    GVIDMov_Reader *r = gvid_mov_reader_open(in_path);
    if (!r) return 1;

    int w = 0, h = 0, fnum = 0, fden = 0;
    int64_t nf = 0;
    gvid_mov_reader_get_video_info(r, &w, &h, &fnum, &fden, &nf);
    fprintf(stderr, "gvid_mov_unpack: %dx%d  %d/%d fps  nb_frames=%lld\n",
            w, h, fnum, fden, (long long)nf);

    GVIDMov_Metadata meta;
    if (gvid_mov_reader_get_metadata(r, &meta) == 0) {
        fprintf(stderr, "  codec=%s quality=%d cfa=%s bits=%d black=%d white=%d\n",
                meta.codec_version ? meta.codec_version : "?",
                meta.quality, meta.cfa_pattern ? meta.cfa_pattern : "?",
                meta.bit_depth, meta.black_level, meta.white_level);
    }

    int idx = 0;
    while (1) {
        const uint8_t *bytes = NULL;
        size_t n = 0;
        int64_t ts_ns = 0;
        int rc = gvid_mov_reader_next_frame(r, &bytes, &n, &ts_ns);
        if (rc == AVERROR_EOF) break;
        if (rc < 0) { gvid_mov_reader_close(r); return 1; }

        char out_path[1024];
        int length = snprintf(out_path, sizeof(out_path), "%s/frame_%04d.gpr", out_dir, idx);
        if (length < 0 || (size_t)length >= sizeof(out_path)) {
            gvid_mov_reader_close(r); return 1;
        }
        if (write_file(out_path, bytes, n) != 0) { gvid_mov_reader_close(r); return 1; }
        idx++;
    }
    fprintf(stderr, "gvid_mov_unpack: wrote %d frames to %s\n", idx, out_dir);
    gvid_mov_reader_close(r);
    return idx > 0 && (nf <= 0 || idx == nf) ? 0 : 1;
}
