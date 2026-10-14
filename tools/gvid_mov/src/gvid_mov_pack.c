/* gvid_mov_pack — directory-of-.gpr → single .mov GVIDMov container.
 *
 * Usage:
 *   gvid_mov_pack [options] INPUT_DIR OUTPUT.mov
 *
 *   INPUT_DIR     directory containing .gpr files; sorted lexically.
 *   OUTPUT.mov    GVIDMov-format MOV.
 *
 * Options (all have defaults):
 *   --fps N            output frame rate            (default 24)
 *   --width N          override frame width         (default: from first GPR header)
 *   --height N         override frame height        (default: from first GPR header)
 *   --quality N        Q0..Q5 metadata tag          (default 3)
 *   --cfa STR          CFA pattern metadata         (default "RGGB")
 *   --bit-depth N      14 or 16                     (default 14)
 *   --black-level N    metadata                     (default 1008)
 *   --white-level N    metadata                     (default 15892)
 *   --encoder-settings JSON   passthrough JSON blob
 *   --source-dng PATH         passthrough path
 *   --codec-version STR       (default "vc5/2.0+gpr")
 *
 * Each .gpr file's first 48 bytes are the FUSED_HEADER (see fused_encode.h);
 * pack uses width/height/decimate from there for the OUTPUT bayer dims unless
 * the caller overrides.
 */

#define _POSIX_C_SOURCE 200809L

#include "gvid_mov.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static uint32_t le32(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Read entire file into a freshly-allocated buffer. Caller frees. */
static uint8_t *slurp(const char *path, size_t *out_n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0)  { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf)    { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    if (out_n) *out_n = (size_t)sz;
    return buf;
}

/* Parse the FUSED_HEADER to recover output dimensions (decimated bayer). */
static int parse_fused_dims(const uint8_t *buf, size_t n,
                            int *out_w, int *out_h) {
    if (n < 48) return -1;
    if (le32(buf) != 0x44535546u) return -2;     /* 'FUSD' */
    uint32_t w   = le32(buf + 8);
    uint32_t h   = le32(buf + 12);
    uint32_t dec = le32(buf + 44);
    /* Match the native decoder: only the value 2 requests decimation. */
    dec = dec == 2 ? 2 : 1;
    *out_w = (int)(w / dec);
    *out_h = (int)(h / dec);
    return 0;
}

static int has_suffix_ci(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* List directory, return sorted array of allocated full paths to .gpr files.
   Caller frees each element + the array. */
static char **list_gpr_files(const char *dir, int *out_n) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); return NULL; }
    size_t cap = 64, n = 0;
    char **arr = malloc(cap * sizeof(char *));
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!has_suffix_ci(de->d_name, ".gpr")) continue;
        if (n == cap) { cap *= 2; arr = realloc(arr, cap * sizeof(char *)); }
        size_t fl = strlen(dir) + 1 + strlen(de->d_name) + 1;
        char *full = malloc(fl);
        snprintf(full, fl, "%s/%s", dir, de->d_name);
        arr[n++] = full;
    }
    closedir(d);
    qsort(arr, n, sizeof(char *), cmp_str);
    *out_n = (int)n;
    return arr;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options] INPUT_DIR OUTPUT.mov\n"
        "  --fps N                 (default 24)\n"
        "  --width N               override (default: from GPR header)\n"
        "  --height N              override (default: from GPR header)\n"
        "  --quality N             Q0..Q5 metadata tag (default 3)\n"
        "  --cfa STR               (default RGGB)\n"
        "  --bit-depth N           14 or 16 (default 14)\n"
        "  --black-level N         (default 1008)\n"
        "  --white-level N         (default 15892)\n"
        "  --encoder-settings STR  JSON blob\n"
        "  --source-dng PATH       traceability\n"
        "  --codec-version STR     (default vc5/2.0+gpr)\n",
        argv0);
}

int main(int argc, char **argv) {
    int fps = 24;
    int w_override = 0, h_override = 0;
    int quality = 3;
    int bit_depth = 14;
    int black = 1008;
    int white = 15892;
    const char *cfa = "RGGB";
    const char *encoder_settings = NULL;
    const char *source_dng = NULL;
    const char *codec_version = "vc5/2.0+gpr";
    const char *in_dir = NULL;
    const char *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEED(n)  if (i + 1 >= argc) { fprintf(stderr, "%s needs an argument\n", a); return 2; } const char *val = argv[++i]; (void)val
        if      (!strcmp(a, "--fps"))               { NEED(1); fps = atoi(val); }
        else if (!strcmp(a, "--width"))             { NEED(1); w_override = atoi(val); }
        else if (!strcmp(a, "--height"))            { NEED(1); h_override = atoi(val); }
        else if (!strcmp(a, "--quality"))           { NEED(1); quality = atoi(val); }
        else if (!strcmp(a, "--cfa"))               { NEED(1); cfa = val; }
        else if (!strcmp(a, "--bit-depth"))         { NEED(1); bit_depth = atoi(val); }
        else if (!strcmp(a, "--black-level"))       { NEED(1); black = atoi(val); }
        else if (!strcmp(a, "--white-level"))       { NEED(1); white = atoi(val); }
        else if (!strcmp(a, "--encoder-settings"))  { NEED(1); encoder_settings = val; }
        else if (!strcmp(a, "--source-dng"))        { NEED(1); source_dng = val; }
        else if (!strcmp(a, "--codec-version"))     { NEED(1); codec_version = val; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (a[0] != '-') {
            if      (!in_dir)   in_dir = a;
            else if (!out_path) out_path = a;
            else { fprintf(stderr, "extra positional: %s\n", a); usage(argv[0]); return 2; }
        }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
        #undef NEED
    }
    if (!in_dir || !out_path) { usage(argv[0]); return 2; }

    int n_files = 0;
    char **paths = list_gpr_files(in_dir, &n_files);
    if (!paths || n_files == 0) {
        fprintf(stderr, "no .gpr files in %s\n", in_dir);
        return 1;
    }
    fprintf(stderr, "gvid_mov_pack: %d frames from %s\n", n_files, in_dir);

    /* Determine output dims from first file unless overridden. */
    size_t first_n = 0;
    uint8_t *first = slurp(paths[0], &first_n);
    if (!first) { fprintf(stderr, "read fail: %s\n", paths[0]); return 1; }
    int dim_w = 0, dim_h = 0;
    if (parse_fused_dims(first, first_n, &dim_w, &dim_h) != 0) {
        fprintf(stderr, "bad FUSED header in %s\n", paths[0]);
        free(first);
        return 1;
    }
    if (w_override) dim_w = w_override;
    if (h_override) dim_h = h_override;
    fprintf(stderr, "  output dims: %dx%d  fps=%d\n", dim_w, dim_h, fps);

    GVIDMov_Metadata meta = {
        .codec_version    = codec_version,
        .quality          = quality,
        .cfa_pattern      = cfa,
        .bit_depth        = bit_depth,
        .black_level      = black,
        .white_level      = white,
        .encoder_settings = encoder_settings,
        .source_dng_path  = source_dng,
        .color_matrix     = NULL,
    };

    GVIDMov_Writer *w = gvid_mov_writer_create(out_path, dim_w, dim_h, fps, 1, &meta);
    if (!w) { fprintf(stderr, "writer_create failed\n"); free(first); return 1; }

    /* First frame uses the already-loaded buffer. */
    int rc = gvid_mov_writer_add_frame(w, first, first_n, 0, NULL);
    free(first);
    if (rc < 0) { gvid_mov_writer_close(w); return 1; }

    for (int i = 1; i < n_files; i++) {
        size_t sz = 0;
        uint8_t *buf = slurp(paths[i], &sz);
        if (!buf) { fprintf(stderr, "read fail: %s\n", paths[i]); gvid_mov_writer_close(w); return 1; }
        int64_t ts_ns = (int64_t)i * 1000000000LL / fps;
        rc = gvid_mov_writer_add_frame(w, buf, sz, ts_ns, NULL);
        free(buf);
        if (rc < 0) { fprintf(stderr, "add_frame %d failed\n", i); gvid_mov_writer_close(w); return 1; }
        if ((i % 25) == 0) fprintf(stderr, "  ... %d/%d\n", i + 1, n_files);
    }
    if (gvid_mov_writer_close(w) < 0) return 1;
    fprintf(stderr, "gvid_mov_pack: wrote %s  (%d frames)\n", out_path, n_files);

    for (int i = 0; i < n_files; i++) free(paths[i]);
    free(paths);
    return 0;
}
