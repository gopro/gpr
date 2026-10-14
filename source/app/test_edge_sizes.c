/* Test encoder at edge-case image sizes — tiny / common video / 50 MP.
 * Runs both split and inline+stripe modes and verifies all return success.
 * Useful for regression checks when refactoring the wavelet boundary handling.
 *
 * Build:
 *   clang -O2 -o /tmp/test_edge_sizes source/app/test_edge_sizes.c \
 *     build/source/lib/vc5_encoder/libvc5_encoder.a \
 *     build/source/lib/vc5_common/libvc5_common.a -lpthread
 *
 * Run:
 *   /tmp/test_edge_sizes                            # split mode
 *   FUSED_INLINE_TOKENIZE=1 /tmp/test_edge_sizes    # inline+stripe mode
 *
 * Sizes tested: 256x256, VGA, Full HD, 4K UHD, HERO10 23 MP, 720p,
 * NTSC SD, MISSION 1 50 MP. Random Bayer data compresses to ~37 %
 * (much worse than real photos that have spatial correlation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int gpr_encode_fused(const unsigned char *raw, size_t sz,
    int w, int h, int pf, int q, unsigned char **out, size_t *out_sz);

static unsigned char *make_raw(int w, int h, uint32_t seed) {
    size_t sz = (size_t)w * h * 2;
    unsigned char *d = malloc(sz);
    uint16_t *p = (uint16_t *)d;
    uint32_t x = seed;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (x >> 4) & 0x3FFF;
    }
    return d;
}

int main(void) {
    struct { int w, h; const char *label; } cases[] = {
        {  256,  256, "tiny 256x256"             },
        {  640,  480, "VGA"                       },
        { 1920, 1080, "Full HD"                   },
        { 3840, 2160, "4K UHD"                    },
        { 5568, 4176, "HERO10 23 MP"              },
        { 1280,  720, "720p"                      },
        {  720,  480, "NTSC SD"                   },
        { 8688, 5800, "MISSION 1 50 MP"           },
        {0, 0, NULL}
    };

    int total = 0, passed = 0;
    for (int i = 0; cases[i].label; i++) {
        int w = cases[i].w, h = cases[i].h;
        unsigned char *d = make_raw(w, h, 0xABCD0000u + i);
        unsigned char *o = NULL; size_t os = 0;
        int rc = gpr_encode_fused(d, (size_t)w*h*2, w, h, 1, 3, &o, &os);
        total++;
        if (rc == 0 && os > 0) {
            passed++;
            fprintf(stderr, "  PASS  %-20s  %5dx%-5d  vc5=%zu bytes (%.1f%% of raw)\n",
                    cases[i].label, w, h, os, 100.0 * os / ((size_t)w*h*2));
        } else {
            fprintf(stderr, "  FAIL  %-20s  %dx%d  rc=%d  size=%zu\n",
                    cases[i].label, w, h, rc, os);
        }
        free(d);
        if (o) free(o);
    }
    fprintf(stderr, "\n%d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
