/*! @file ans_joint.c
 *  @brief Joint RLV ANS coder — single symbol per coefficient.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "ans_joint.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

#ifdef __aarch64__
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/* NEON vectorization of the rANS decode was tested and reverted —
   ARM lacks gather instructions, making scalar table lookups faster.
   See docs/future-ideas.md for details. */

#define RANS_BYTE_L (1u << 23)

/* Run class encoding: class → (min_run, extra_bits)
   Class 0: run=0 (0 bits)      Class 4: run=4-7 (2 bits)
   Class 1: run=1 (0 bits)      Class 5: run=8-15 (3 bits)
   Class 2: run=2 (0 bits)      Class 6: run=16-31 (4 bits)
   Class 3: run=3 (0 bits)      Class 7: run=32-287 (8 bits) */
/* Contiguous exponentially spaced run classes — no gaps */
static const int run_class_min[JANS_RUN_CLASSES] = {0, 1, 2, 3, 4,  8, 16, 32,  64,  128};
static const int run_class_bits[JANS_RUN_CLASSES] = {0, 0, 0, 0, 2,  3,  4,  5,   6,    7};

/* Fast LUT-based run_to_class: O(1) lookup for runs 0-255 */
static uint8_t run_class_lut[256];
static uint8_t run_resid_lut[256];
static int run_lut_initialized = 0;

static void init_run_lut(void) {
    if (run_lut_initialized) return;
    for (int r = 0; r < 256; r++) {
        for (int c = JANS_RUN_CLASSES - 1; c >= 0; c--) {
            if (r >= run_class_min[c]) {
                run_class_lut[r] = (uint8_t)c;
                run_resid_lut[r] = (uint8_t)(r - run_class_min[c]);
                break;
            }
        }
    }
    run_lut_initialized = 1;
}

static inline int run_to_class(int run, int *residual) {
    if (__builtin_expect(run < 256, 1)) {
        *residual = run_resid_lut[run];
        return run_class_lut[run];
    }
    /* Fallback for run >= 256 (rare) */
    for (int c = JANS_RUN_CLASSES - 1; c >= 0; c--) {
        if (run >= run_class_min[c]) {
            *residual = run - run_class_min[c];
            return c;
        }
    }
    *residual = run;
    return 0;
}

static int class_to_run(int cls, int residual) {
    return run_class_min[cls] + residual;
}

/* Mag class encoding: class → (min_mag, extra_bits)
   Class 0: mag=0 (sentinel)     Class 8: mag=8-15 (3 bits)
   Class 1: mag=1 (0 bits)       Class 9: mag=16-31 (4 bits)
   Class 2: mag=2 (0 bits)       Class 10: mag=32-63 (5 bits)
   Class 3: mag=3 (0 bits)       Class 11: mag=64-127 (6 bits)
   Class 4: mag=4 (0 bits)       Class 12: mag=128-255 (7 bits)
   Class 5: mag=5 (0 bits)       Class 13: mag=256-511 (8 bits)
   Class 6: mag=6 (0 bits)       Class 14: mag=512-1023 (9 bits)
   Class 7: mag=7 (0 bits)       Class 15: mag=1024+ (10 bits) */
static const int mag_class_min[JANS_MAG_CLASSES] = {0,1,2,3,4,5,6,7,8,16,32,64,128,256,512,1024};
static const int mag_class_bits[JANS_MAG_CLASSES] = {0,0,0,0,0,0,0,0,3,4,5,6,7,8,9,10};

/* Per-symbol bit lengths for the inline-tokenize merged-write step:
   sym_run_bits[sym] = run_class_bits[sym / JANS_MAG_CLASSES]
   sym_total_bits[sym] = run_class_bits[rc] + mag_class_bits[mc] + 1
   (the +1 is the sign bit, always present for mc>0 = nonzero coef).
   For sym with mc==0 (pure run, no value), total = run_class_bits[rc] only,
   but those are only used in the zero-run flushes which call bitbuf_write
   with run_class_bits[rc] directly — not via these tables. */
static uint8_t sym_run_bits[JANS_NUM_SYMBOLS + 1];
static uint8_t sym_total_bits[JANS_NUM_SYMBOLS + 1];
static int sym_bits_initialized = 0;

static void init_sym_bits(void) {
    if (sym_bits_initialized) return;
    for (int rc = 0; rc < JANS_RUN_CLASSES; rc++) {
        for (int mc = 0; mc < JANS_MAG_CLASSES; mc++) {
            int sym = rc * JANS_MAG_CLASSES + mc;
            int rb = run_class_bits[rc];
            int mb = mag_class_bits[mc];
            sym_run_bits[sym] = (uint8_t)rb;
            sym_total_bits[sym] = (uint8_t)(rb + mb + 1);
        }
    }
    sym_bits_initialized = 1;
}

/* Fast LUT-based mag_to_class: O(1) for magnitudes 0-2047, O(N) fallback for larger */
static uint8_t mag_class_lut[2048];
static uint16_t mag_resid_lut[2048];
static int mag_lut_initialized = 0;

static void init_mag_lut(void) {
    if (mag_lut_initialized) return;
    for (int m = 0; m < 2048; m++) {
        for (int c = JANS_MAG_CLASSES - 1; c >= 0; c--) {
            if (m >= mag_class_min[c]) {
                mag_class_lut[m] = (uint8_t)c;
                mag_resid_lut[m] = (uint16_t)(m - mag_class_min[c]);
                break;
            }
        }
    }
    mag_lut_initialized = 1;
}

static inline int mag_to_class(int mag, int *residual) {
    if (__builtin_expect(mag < 2048, 1)) {
        *residual = mag_resid_lut[mag];
        return mag_class_lut[mag];
    }
    for (int c = JANS_MAG_CLASSES - 1; c >= 0; c--) {
        if (mag >= mag_class_min[c]) {
            *residual = mag - mag_class_min[c];
            return c;
        }
    }
    *residual = mag;
    return 0;
}

static int class_to_mag(int cls, int residual) {
    return mag_class_min[cls] + residual;
}

/* --- rANS core --- */

/* Pass rans_ptr by value, return updated value. Lets the compiler keep
   rans_ptr in a register across the 4 unrolled rans_enc_put calls per
   loop iteration — no spill through &rans_ptr. */
static inline uint8_t *rans_enc_put(uint32_t *state, uint8_t *ptr,
                                    uint32_t start, uint32_t freq, uint32_t rcp_freq) {
    uint32_t x = *state;
    uint32_t x_max = ((RANS_BYTE_L >> JANS_TABLE_BITS) << 8) * freq;
    while (x >= x_max) { *ptr++ = (uint8_t)(x & 0xFF); x >>= 8; }

    /* Division-free encode with unified path for all freq values.
       build_tables stores 0xFFFFFFFF for freq=1 so the same formula
       produces q=x, mod=0 after the off-by-one correction (verified). */
    uint32_t q = (uint32_t)(((uint64_t)x * rcp_freq) >> 32);
    uint32_t mod = x - q * freq;
    if (mod >= freq) { q++; mod -= freq; }
    *state = (q << JANS_TABLE_BITS) + mod + start;
    return ptr;
}

static inline int rans_dec_renorm(uint32_t *state, const uint8_t **pptr,
                                  const uint8_t *end) {
    while (*state < RANS_BYTE_L) {
        if (*pptr >= end) return -1;
        *state = (*state << 8) | **pptr;
        (*pptr)++;
    }
    return 0;
}

/* --- Bit buffer for residual bits ---
 *
 * Accumulator design: bits accumulate in a 64-bit register, only whole bytes
 * are emitted to memory. Eliminates the read-modify-write dependency of the
 * old "OR into 32-bit word at buf[byte_pos]" approach, which forced the CPU
 * to load each word it had just stored — a serial chain on the hot path.
 *
 * Also skips the memset(buf, 0, cap) at init since we write whole bytes
 * rather than OR'ing into pre-zeroed memory.
 *
 * Call bitbuf_size(bb) at end of band to flush trailing partial byte.
 * Output byte stream is bit-identical to the previous implementation.
 */
typedef struct {
    uint8_t *buf;
    size_t   capacity;
    size_t   byte_pos;     /* bytes already emitted to buf */
    uint64_t accum;        /* bits not yet emitted, LSB-aligned */
    int      accum_bits;   /* number of valid bits in accum (0..63) */
} BITBUF;

static void bitbuf_init(BITBUF *bb, uint8_t *buf, size_t cap) {
    bb->buf = buf; bb->capacity = cap;
    bb->byte_pos = 0;
    bb->accum = 0;
    bb->accum_bits = 0;
    /* No memset needed — bytes are overwritten, not OR'd into. */
}

static inline __attribute__((always_inline))
void bitbuf_write(BITBUF *bb, uint32_t value, int bits) {
    if (bits == 0) return;
    uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1);
    bb->accum |= ((uint64_t)(value & mask)) << bb->accum_bits;
    bb->accum_bits += bits;
    /* 32-bit drain: one unaligned store covers 4 tokens of typical bit-width.
       Typical token writes ~10 bits, so we drain every 3rd-4th call instead
       of once per byte. Reduces store-port traffic ~4x in the encode hot loop. */
    if (bb->accum_bits >= 32 && bb->byte_pos + 4 <= bb->capacity) {
        uint32_t word = (uint32_t)bb->accum;
        memcpy(bb->buf + bb->byte_pos, &word, 4);  /* ARM64 + clang inline this to STR */
        bb->byte_pos += 4;
        bb->accum >>= 32;
        bb->accum_bits -= 32;
    }
    /* Drain any whole bytes left over (0-3 bytes when bits exceeded 32). */
    while (bb->accum_bits >= 8) {
        if (bb->byte_pos < bb->capacity) {
            bb->buf[bb->byte_pos++] = (uint8_t)bb->accum;
        }
        bb->accum >>= 8;
        bb->accum_bits -= 8;
    }
}

/* Returns the total byte size, flushing any trailing partial byte into buf. */
static size_t bitbuf_size(BITBUF *bb) {
    while (bb->accum_bits > 0) {
        if (bb->byte_pos < bb->capacity) {
            bb->buf[bb->byte_pos++] = (uint8_t)bb->accum;
        }
        bb->accum >>= 8;
        bb->accum_bits -= (bb->accum_bits >= 8) ? 8 : bb->accum_bits;
    }
    return bb->byte_pos;
}

static inline __attribute__((always_inline))
uint32_t bitbuf_read(const uint8_t *buf, size_t buf_size,
                     size_t *byte_pos, int *bit_pos, int bits) {
    if (bits == 0) return 0;
    /* Fast path: single unaligned 64-bit load covers up to 57 bits.
       Use memcpy for strict aliasing compliance — compiler generates LDR. */
    if (__builtin_expect(*byte_pos + 7 < buf_size, 1)) {
        uint64_t word;
        memcpy(&word, buf + *byte_pos, 8);
        uint32_t value = (uint32_t)(word >> *bit_pos) & ((1u << bits) - 1);
        int new_bit = *bit_pos + bits;
        *byte_pos += new_bit >> 3;
        *bit_pos = new_bit & 7;
        return value;
    }
    /* Fallback: 32-bit load for near end of buffer */
    if (*byte_pos + 3 < buf_size) {
        uint32_t word;
        memcpy(&word, buf + *byte_pos, 4);
        uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1);
        uint32_t value = (word >> *bit_pos) & mask;
        int new_bit = *bit_pos + bits;
        *byte_pos += new_bit >> 3;
        *bit_pos = new_bit & 7;
        return value;
    }
    /* Slow fallback for end of buffer */
    uint32_t value = 0;
    for (int i = 0; i < bits; i++) {
        if (*byte_pos < buf_size) {
            value |= ((buf[*byte_pos] >> *bit_pos) & 1) << i;
            (*bit_pos)++;
            if (*bit_pos >= 8) { *bit_pos = 0; (*byte_pos)++; }
        }
    }
    return value;
}

/* --- Bump allocator for embedded: one malloc per encode call --- */

typedef struct {
    uint8_t *base;
    size_t   capacity;
    size_t   offset;
} JANS_ARENA;

static int arena_init(JANS_ARENA *a, size_t capacity) {
    a->base = (uint8_t *)malloc(capacity);
    a->capacity = capacity;
    a->offset = 0;
    return a->base ? 0 : -1;
}

static void *arena_alloc(JANS_ARENA *a, size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;
    if (a->offset + size > a->capacity) return NULL;
    void *ptr = a->base + a->offset;
    a->offset += size;
    return ptr;
}

static void arena_free(JANS_ARENA *a) {
    free(a->base);
    a->base = NULL;
    a->capacity = 0;
    a->offset = 0;
}

/* Saturating frequency increment.

   table.freq[] is uint16_t. Mission 1 native12 inline stripes can put more
   than 65k coefficients in a single symbol bucket, so plain ++ can wrap the
   modeled frequency to zero before normalize_freq(). Saturating preserves a
   valid nonzero model; the normalized table's max-symbol correction absorbs
   the small count distortion. */
#define JANS_FREQ_INC(f) do { if ((f) != 0xFFFFu) (f)++; } while (0)

/* --- Normalize and build tables --- */

static void normalize_freq(uint16_t *freq, int n) {
    uint32_t total = 0;
    for (int i = 0; i < n; i++) total += freq[i];
    if (total == 0) { freq[0] = JANS_TABLE_SIZE; return; }
    uint32_t scaled_total = 0;
    uint16_t scaled[JANS_NUM_SYMBOLS + 1];
    for (int i = 0; i < n; i++) {
        if (freq[i] > 0) {
            uint32_t s = (uint32_t)freq[i] * JANS_TABLE_SIZE / total;
            if (s == 0) s = 1;
            scaled[i] = (uint16_t)s;
        } else scaled[i] = 0;
        scaled_total += scaled[i];
    }
    int32_t diff = (int32_t)JANS_TABLE_SIZE - (int32_t)scaled_total;
    int max_idx = 0;
    for (int i = 1; i < n; i++) if (scaled[i] > scaled[max_idx]) max_idx = i;
    scaled[max_idx] = (uint16_t)((int32_t)scaled[max_idx] + diff);
    memcpy(freq, scaled, n * sizeof(uint16_t));
}

/* Encoder-only path: just cum_freq + rcp_freq. Skips the 3 × 2048-entry
   decode tables that build_tables fills (decode_sym/decode_fast/decode_info)
   — those are only read by the decoder. */
static void build_encode_tables(JANS_TABLE *t, int n) {
    t->cum_freq[0] = 0;
    for (int i = 1; i < n; i++) t->cum_freq[i] = t->cum_freq[i-1] + t->freq[i-1];
    for (int i = 0; i < n; i++) {
        uint32_t rcp;
        if (t->freq[i] > 1)
            rcp = (uint32_t)(((uint64_t)1 << 32) / t->freq[i]);
        else if (t->freq[i] == 1)
            rcp = 0xFFFFFFFFu;
        else
            rcp = 0;
        t->rcp_freq[i] = rcp;
        /* Packed encode entry: rans_enc_put can do 1 cacheline-load per
           symbol instead of 3 scattered loads (Pi A76 perf showed those
           4 × per-stream loads at ~10% each in jans_inline_emit_blob). */
        t->enc[i].freq = t->freq[i];
        t->enc[i].cum_freq = t->cum_freq[i];
        t->enc[i].rcp_freq = rcp;
    }
}

static void build_tables(JANS_TABLE *t, int n) {
    t->cum_freq[0] = 0;
    for (int i = 1; i < n; i++) t->cum_freq[i] = t->cum_freq[i-1] + t->freq[i-1];
    int sym = 0;
    for (int i = 0; i < JANS_TABLE_SIZE; i++) {
        while (sym < n-1 && i >= t->cum_freq[sym] + t->freq[sym]) sym++;
        t->decode_sym[i] = (uint16_t)sym;
        /* Packed decode entry: sym + freq + cum_freq in one lookup */
        t->decode_fast[i].sym = (uint16_t)sym;
        t->decode_fast[i].freq = t->freq[sym];
        t->decode_fast[i].cum_freq = t->cum_freq[sym];
    }
    /* Precompute ultra-packed decode info per table slot.
       Eliminates per-token: sym/16, sym%16, class_to_run, class_to_mag lookups. */
    for (int i = 0; i < JANS_TABLE_SIZE; i++) {
        int sym = t->decode_fast[i].sym;
        int rc = sym / JANS_MAG_CLASSES;
        int mc = sym % JANS_MAG_CLASSES;
        t->decode_info[i].freq = t->decode_fast[i].freq;
        t->decode_info[i].cum_freq = t->decode_fast[i].cum_freq;
        t->decode_info[i].run_bits = (uint8_t)run_class_bits[rc];
        t->decode_info[i].mag_bits = (mc > 0) ? (uint8_t)mag_class_bits[mc] : 0;
        t->decode_info[i].has_value = (mc > 0) ? 1 : 0;
        t->decode_info[i].total_bits = t->decode_info[i].run_bits + t->decode_info[i].mag_bits + t->decode_info[i].has_value;
        t->decode_info[i].run_min = (uint16_t)run_class_min[rc];
        t->decode_info[i].mag_min = (mc > 0) ? (uint16_t)mag_class_min[mc] : 0;
    }
    /* Precompute reciprocals for division-free encode (Giesen's trick).
       rcp_freq[i] = floor(2^32 / freq[i]). Then x / freq ≈ (x * rcp) >> 32,
       corrected by an off-by-one fix in rans_enc_put.

       Special case: freq=1 wants rcp=2^32 which overflows uint32. Use
       0xFFFFFFFF (= 2^32 - 1) instead: the correction loop still yields
       q=x, mod=0, so the result matches the freq=1 ideal. This lets us
       eliminate the `if (freq == 1)` branch from the hot encoder loop. */
    for (int i = 0; i < n; i++) {
        if (t->freq[i] > 1)
            t->rcp_freq[i] = (uint32_t)(((uint64_t)1 << 32) / t->freq[i]);
        else if (t->freq[i] == 1)
            t->rcp_freq[i] = 0xFFFFFFFFu;
        else
            t->rcp_freq[i] = 0;
    }
}

/* --- Encode --- */

int jans_encode_band(uint8_t *out_buf, size_t out_capacity,
                     const int32_t *data, int width, int height, int pitch) {
    int pitch_elems = pitch / sizeof(int32_t);
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > (size_t)(INT32_MAX / 2)) return -1;

    init_run_lut();
    init_mag_lut(); init_sym_bits();

    /* Single allocation for all encode buffers (embedded-friendly).
       tokens: max_tokens × 2 bytes, resid: pixels × 2, rans: pixels × 2 + 4K */
    size_t max_tokens = pixels + height + 16;
    size_t resid_cap = pixels * 2;
    size_t rans_cap = pixels * 2 + 4096;
    size_t arena_size = max_tokens * sizeof(uint16_t) + resid_cap + rans_cap + 64;
    JANS_ARENA arena;
    if (arena_init(&arena, arena_size) != 0) return -1;

    uint16_t *tokens = (uint16_t *)arena_alloc(&arena, max_tokens * sizeof(uint16_t));
    uint8_t *resid_buf = (uint8_t *)arena_alloc(&arena, resid_cap);
    if (!tokens || !resid_buf) { arena_free(&arena); return -1; }

    BITBUF bb;
    bitbuf_init(&bb, resid_buf, resid_cap);

    JANS_TABLE table;
    memset(&table, 0, sizeof(table));
    int token_count = 0;

    for (int row = 0; row < height; row++) {
        const int32_t *rowptr = data + row * pitch_elems;
        int run = 0;
        for (int col = 0; col < width; col++) {
            int32_t val = rowptr[col];
            if (val == 0) { run++; continue; }

            int32_t mag = (val < 0) ? -val : val;

            /* Emit long runs as run-only tokens (mag_class=0) */
            while (run >= 256) {
                int rr; int rc = run_to_class(255, &rr);
                int sym = rc * JANS_MAG_CLASSES + 0;
                JANS_FREQ_INC(table.freq[sym]);
                tokens[token_count++] = (uint16_t)sym;
                bitbuf_write(&bb, rr, run_class_bits[rc]);
                run -= 255;
            }

            int run_resid, mag_resid;
            int rc = run_to_class(run, &run_resid);
            int mc = mag_to_class(mag, &mag_resid);

            int sym = rc * JANS_MAG_CLASSES + mc;
            JANS_FREQ_INC(table.freq[sym]);
            tokens[token_count++] = (uint16_t)sym;

            /* Merged residual write: run_resid + mag_resid + sign in one call */
            {
                int rb = run_class_bits[rc], mb = mag_class_bits[mc];
                uint32_t merged = (uint32_t)run_resid;
                if (mc > 0) {
                    merged |= ((uint32_t)mag_resid << rb);
                    merged |= ((val < 0) ? 1u : 0u) << (rb + mb);
                    bitbuf_write(&bb, merged, rb + mb + 1);
                } else {
                    bitbuf_write(&bb, merged, rb);
                }
            }

            run = 0;
        }
        /* Trailing zeros: run-only token */
        if (run > 0) {
            while (run > 0) {
                int actual = (run > 255) ? 255 : run;
                int rr; int rc = run_to_class(actual, &rr);
                int sym = rc * JANS_MAG_CLASSES + 0;
                JANS_FREQ_INC(table.freq[sym]);
                tokens[token_count++] = (uint16_t)sym;
                bitbuf_write(&bb, rr, run_class_bits[rc]);
                run -= actual;
            }
        }
    }

    size_t resid_size = bitbuf_size(&bb);

    /* Normalize and build ANS table */
    normalize_freq(table.freq, JANS_NUM_SYMBOLS);
    build_encode_tables(&table, JANS_NUM_SYMBOLS);
    table.initialized = 1;

    /* rANS encode tokens in reverse (from pre-allocated arena) */
    uint8_t *rans_buf = (uint8_t *)arena_alloc(&arena, rans_cap);
    if (!rans_buf) { arena_free(&arena); return -1; }

    uint8_t *rans_ptr = rans_buf;
    uint32_t state = RANS_BYTE_L;

    for (int i = token_count - 1; i >= 0; i--) {
        int sym = tokens[i];
        rans_ptr = rans_enc_put(&state, rans_ptr,
                     table.cum_freq[sym], table.freq[sym], table.rcp_freq[sym]);
    }
    *rans_ptr++ = (uint8_t)(state >> 0);
    *rans_ptr++ = (uint8_t)(state >> 8);
    *rans_ptr++ = (uint8_t)(state >> 16);
    *rans_ptr++ = (uint8_t)(state >> 24);

    size_t rans_size = rans_ptr - rans_buf;

    /* Reverse rANS buffer */
    for (size_t i = 0; i < rans_size / 2; i++) {
        uint8_t t = rans_buf[i]; rans_buf[i] = rans_buf[rans_size-1-i];
        rans_buf[rans_size-1-i] = t;
    }

    /* Serialize frequency table (tokens no longer needed — arena reclaims) */
    uint8_t freq_buf[JANS_NUM_SYMBOLS * 2];
    for (int i = 0; i < JANS_NUM_SYMBOLS; i++) {
        freq_buf[i*2] = (uint8_t)(table.freq[i] >> 8);
        freq_buf[i*2+1] = (uint8_t)(table.freq[i]);
    }
    int freq_size = JANS_NUM_SYMBOLS * 2;

    /* Pack: [token_count:4][freq_size:4][rans_size:4][resid_size:4]
             [freq_data][rans_data][resid_data] */
    size_t total = 16 + freq_size + rans_size + resid_size;
    if (total > out_capacity) { arena_free(&arena); return -1; }

    uint8_t *p = out_buf;
    *p++ = (token_count>>24)&0xFF; *p++ = (token_count>>16)&0xFF;
    *p++ = (token_count>>8)&0xFF;  *p++ = token_count&0xFF;
    *p++ = (freq_size>>24)&0xFF;   *p++ = (freq_size>>16)&0xFF;
    *p++ = (freq_size>>8)&0xFF;    *p++ = freq_size&0xFF;
    *p++ = (rans_size>>24)&0xFF;   *p++ = (rans_size>>16)&0xFF;
    *p++ = (rans_size>>8)&0xFF;    *p++ = rans_size&0xFF;
    *p++ = (resid_size>>24)&0xFF;  *p++ = (resid_size>>16)&0xFF;
    *p++ = (resid_size>>8)&0xFF;   *p++ = resid_size&0xFF;
    memcpy(p, freq_buf, freq_size); p += freq_size;
    memcpy(p, rans_buf, rans_size); p += rans_size;
    memcpy(p, resid_buf, resid_size);

    arena_free(&arena);  /* Single free for all encode buffers */
    return (int)total;
}

/* --- Decode --- */

int jans_decode_band(const uint8_t *in_buf, size_t in_size,
                     int32_t *data, int width, int height, int pitch) {
    if (in_size < 16) return -1;
    int pitch_elems = pitch / sizeof(int32_t);

    const uint8_t *p = in_buf;
    int token_count = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int freq_size   = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int rans_size   = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int resid_size  = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;

    if (token_count < 0 || freq_size < 0 || rans_size < 4 || resid_size < 0) return -1;
    if ((size_t)16 + (size_t)freq_size + (size_t)rans_size + (size_t)resid_size > in_size) return -1;

    /* Validate freq_size is sufficient for the full frequency table */
    if (freq_size < JANS_NUM_SYMBOLS * 2) return -1;

    /* Validate token_count is reasonable for the image dimensions */
    {
        size_t max_reasonable_tokens = (size_t)width * (size_t)height * 2;
        if (max_reasonable_tokens > 0 && (size_t)token_count > max_reasonable_tokens) return -1;
    }

    /* Deserialize frequency table */
    const uint8_t *freq_data = p; p += freq_size;
    JANS_TABLE table;
    memset(&table, 0, sizeof(table));
    for (int i = 0; i < JANS_NUM_SYMBOLS && i*2+1 < freq_size; i++)
        table.freq[i] = ((uint16_t)freq_data[i*2] << 8) | freq_data[i*2+1];
    normalize_freq(table.freq, JANS_NUM_SYMBOLS);

    /* Verify normalization produced a valid table */
    {
        uint32_t freq_sum = 0;
        for (int i = 0; i < JANS_NUM_SYMBOLS; i++)
            freq_sum += table.freq[i];
        if (freq_sum != JANS_TABLE_SIZE) return -1;
    }

    build_tables(&table, JANS_NUM_SYMBOLS);

    const uint8_t *rans_data = p; p += rans_size;
    const uint8_t *rans_end = rans_data + rans_size;
    const uint8_t *resid_data = p;

    /* Init rANS */
    uint32_t state = ((uint32_t)rans_data[0]<<24) | ((uint32_t)rans_data[1]<<16) |
                     ((uint32_t)rans_data[2]<<8) | (uint32_t)rans_data[3];
    const uint8_t *rptr = rans_data + 4;

    /* Clear output */
    for (int row = 0; row < height; row++)
        memset(data + row * pitch_elems, 0, width * sizeof(int32_t));

    size_t resid_byte = 0;
    int resid_bit = 0;
    int row = 0, col = 0;

    for (int t = 0; t < token_count && row < height; t++) {
        /* Decode token */
        uint32_t slot = state & (JANS_TABLE_SIZE - 1);
        int sym = table.decode_sym[slot];
        uint16_t freq = table.freq[sym];
        state = freq * (state >> JANS_TABLE_BITS) + slot - table.cum_freq[sym];
        if (rans_dec_renorm(&state, &rptr, rans_end) != 0) return -1;

        int rc = sym / JANS_MAG_CLASSES;
        int mc = sym % JANS_MAG_CLASSES;

        /* Read run residual bits */
        int run_resid = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit,
                                    run_class_bits[rc]);
        int run = class_to_run(rc, run_resid);

        /* Apply zeros */
        col += run;
        while (col >= width) { col -= width; row++; }

        if (mc > 0 && row < height) {
            /* Read magnitude residual bits */
            int mag_resid = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit,
                                        mag_class_bits[mc]);
            int mag = class_to_mag(mc, mag_resid);

            /* Read sign bit */
            int sign = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit, 1);

            if (col < width)
                data[row * pitch_elems + col] = sign ? -mag : mag;
            col++;
            if (col >= width) { row++; col = 0; }
        }
    }

    return 0;
}

/* ================================================================
   4-way interleaved rANS encode/decode
   ================================================================ */

#define JANS_INTERLEAVE 4

#if defined(JANS_TIMING_DETAIL)
#include <mach/mach_time.h>
static double _jans_ms(void) {
    static double s = 0;
    if (!s) { mach_timebase_info_data_t i; mach_timebase_info(&i); s=(double)i.numer/i.denom/1e6; }
    return mach_absolute_time() * s;
}
static double _jans_tok_ms = 0, _jans_rans_ms = 0, _jans_out_ms = 0;
#endif

int jans_encode_band_x4(uint8_t *out_buf, size_t out_capacity,
                        const int32_t *data, int width, int height, int pitch) {
    int pitch_elems = pitch / sizeof(int32_t);
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > (size_t)(INT32_MAX / 2)) return -1;

    /* Initialize classification LUTs (once) */
    init_run_lut();
    init_mag_lut(); init_sym_bits();

    /* Single allocation for all encode buffers (embedded-friendly) */
    size_t max_tokens = pixels + height + 16;
    size_t resid_cap = pixels * 2;
    size_t rans_cap = pixels * 2 + 4096;
    size_t arena_size = max_tokens * sizeof(uint16_t) + resid_cap + rans_cap + 64;
    JANS_ARENA arena;
    if (arena_init(&arena, arena_size) != 0) return -1;

    uint16_t *tokens = (uint16_t *)arena_alloc(&arena, max_tokens * sizeof(uint16_t));
    uint8_t *resid_buf = (uint8_t *)arena_alloc(&arena, resid_cap);
    if (!tokens || !resid_buf) { arena_free(&arena); return -1; }

    BITBUF bb;
    bitbuf_init(&bb, resid_buf, resid_cap);

    JANS_TABLE table;
    memset(&table, 0, sizeof(table));
    int token_count = 0;

#if defined(JANS_TIMING_DETAIL)
    double _t0 = _jans_ms();
#endif

    /* Hoist BITBUF state into local register variables. The compiler can't
       usually do this through a pointer because aliasing rules forbid it.
       We sync back at end of tokenize. Each non-zero coefficient calls into
       the merged-write path below, which touches accum/accum_bits/byte_pos
       without going through memory. */
    uint64_t bb_accum = bb.accum;
    int      bb_accum_bits = bb.accum_bits;
    size_t   bb_pos = bb.byte_pos;
    uint8_t * const bb_buf = bb.buf;
    const size_t bb_cap = bb.capacity;

    #define BB_WRITE_LOCAL(value, bits) do {                              \
        if ((bits) > 0) {                                                 \
            uint32_t _v = (uint32_t)(value);                              \
            int _b = (bits);                                              \
            uint32_t _mask = (_b >= 32) ? 0xFFFFFFFFu : ((1u << _b) - 1); \
            bb_accum |= ((uint64_t)(_v & _mask)) << bb_accum_bits;        \
            bb_accum_bits += _b;                                          \
            if (bb_accum_bits >= 32 && bb_pos + 4 <= bb_cap) {            \
                uint32_t _w = (uint32_t)bb_accum;                         \
                memcpy(bb_buf + bb_pos, &_w, 4);                          \
                bb_pos += 4;                                              \
                bb_accum >>= 32;                                          \
                bb_accum_bits -= 32;                                      \
            }                                                             \
            while (bb_accum_bits >= 8) {                                  \
                if (bb_pos < bb_cap) bb_buf[bb_pos++] = (uint8_t)bb_accum;\
                bb_accum >>= 8;                                           \
                bb_accum_bits -= 8;                                       \
            }                                                             \
        }                                                                 \
    } while (0)

    for (int row = 0; row < height; row++) {
        const int32_t *rowptr = data + row * pitch_elems;
        int run = 0;
        for (int col = 0; col < width; col++) {
            int32_t val = rowptr[col];
            if (val == 0) { run++; continue; }
            int32_t mag = (val < 0) ? -val : val;
            while (run >= 256) {
                int rr; int rc = run_to_class(255, &rr);
                int sym = rc * JANS_MAG_CLASSES + 0;
                JANS_FREQ_INC(table.freq[sym]);
                tokens[token_count++] = (uint16_t)sym;
                BB_WRITE_LOCAL(rr, run_class_bits[rc]);
                run -= 255;
            }
            int run_resid, mag_resid;
            int rc = run_to_class(run, &run_resid);
            int mc = mag_to_class(mag, &mag_resid);
            int sym = rc * JANS_MAG_CLASSES + mc;
            JANS_FREQ_INC(table.freq[sym]);
            tokens[token_count++] = (uint16_t)sym;
            /* val != 0 here (checked above) -> mag >= 1 -> mc >= 1, so the
               (mc > 0) check that wrapped this block in the original code
               was always true. Inline the always-taken path. */
            {
                int rb = run_class_bits[rc];
                int mb = mag_class_bits[mc];
                uint32_t merged = (uint32_t)run_resid;
                merged |= ((uint32_t)mag_resid << rb);
                merged |= ((val < 0) ? 1u : 0u) << (rb + mb);
                BB_WRITE_LOCAL(merged, rb + mb + 1);
            }
            run = 0;
        }
        if (run > 0) {
            while (run > 0) {
                int actual = (run > 255) ? 255 : run;
                int rr; int rc = run_to_class(actual, &rr);
                int sym = rc * JANS_MAG_CLASSES + 0;
                JANS_FREQ_INC(table.freq[sym]);
                tokens[token_count++] = (uint16_t)sym;
                BB_WRITE_LOCAL(rr, run_class_bits[rc]);
                run -= actual;
            }
        }
    }
    #undef BB_WRITE_LOCAL

    /* Sync local bitbuf state back to bb so bitbuf_size() works */
    bb.accum = bb_accum;
    bb.accum_bits = bb_accum_bits;
    bb.byte_pos = bb_pos;

    size_t resid_size = bitbuf_size(&bb);
#if defined(JANS_TIMING_DETAIL)
    _jans_tok_ms += _jans_ms() - _t0; _t0 = _jans_ms();
#endif
    normalize_freq(table.freq, JANS_NUM_SYMBOLS);
    build_encode_tables(&table, JANS_NUM_SYMBOLS);

    /* 4-way interleaved rANS encode (from pre-allocated arena) */
    uint8_t *rans_buf = (uint8_t *)arena_alloc(&arena, rans_cap);
    if (!rans_buf) { arena_free(&arena); return -1; }

    uint8_t *rans_ptr = rans_buf;
    uint32_t states[JANS_INTERLEAVE];
    for (int s = 0; s < JANS_INTERLEAVE; s++) states[s] = RANS_BYTE_L;

    /* 4-way unrolled rANS encode (backward).
       Each iteration touches 4 *independent* states — no cross-stream data
       dependency — letting the CPU pipeline them in parallel. The compiler
       inlines rans_enc_put, exposing the per-stream UMULL + correction +
       optional renormalize as 4 disjoint chains.
       Stream assignment matches the original `i % 4` mapping for bit-identical
       output: tokens[i] -> states[i % 4]. */
    const JANS_ENC_ENTRY *enc = table.enc;
    int i = token_count - 1;
    /* Align so that the unrolled chunk processes [i, i-1, i-2, i-3] where
       (i % 4) == 3 — then each call below uses a constant stream index.
       Pass rans_ptr by value to keep it in a register across calls. */
    while (i >= 0 && (i & 3) != 3) {
        JANS_ENC_ENTRY e = enc[tokens[i]];
        rans_ptr = rans_enc_put(&states[i & 3], rans_ptr,
                     e.cum_freq, e.freq, e.rcp_freq);
        i--;
    }
    for (; i >= 3; i -= 4) {
        JANS_ENC_ENTRY e3 = enc[tokens[i]];
        JANS_ENC_ENTRY e2 = enc[tokens[i - 1]];
        JANS_ENC_ENTRY e1 = enc[tokens[i - 2]];
        JANS_ENC_ENTRY e0 = enc[tokens[i - 3]];
        rans_ptr = rans_enc_put(&states[3], rans_ptr,
                     e3.cum_freq, e3.freq, e3.rcp_freq);
        rans_ptr = rans_enc_put(&states[2], rans_ptr,
                     e2.cum_freq, e2.freq, e2.rcp_freq);
        rans_ptr = rans_enc_put(&states[1], rans_ptr,
                     e1.cum_freq, e1.freq, e1.rcp_freq);
        rans_ptr = rans_enc_put(&states[0], rans_ptr,
                     e0.cum_freq, e0.freq, e0.rcp_freq);
    }
    while (i >= 0) {
        JANS_ENC_ENTRY e = enc[tokens[i]];
        rans_ptr = rans_enc_put(&states[i & 3], rans_ptr,
                     e.cum_freq, e.freq, e.rcp_freq);
        i--;
    }

    /* Flush 4 states (state 3 first, state 0 last → read state 0 first) */
    for (int s = JANS_INTERLEAVE - 1; s >= 0; s--) {
        *rans_ptr++ = (uint8_t)(states[s] >> 0);
        *rans_ptr++ = (uint8_t)(states[s] >> 8);
        *rans_ptr++ = (uint8_t)(states[s] >> 16);
        *rans_ptr++ = (uint8_t)(states[s] >> 24);
    }

    size_t rans_size = rans_ptr - rans_buf;
#if defined(JANS_TIMING_DETAIL)
    _jans_rans_ms += _jans_ms() - _t0; _t0 = _jans_ms();
#endif

    for (size_t i = 0; i < rans_size / 2; i++) {
        uint8_t t = rans_buf[i]; rans_buf[i] = rans_buf[rans_size-1-i];
        rans_buf[rans_size-1-i] = t;
    }

    uint8_t freq_buf[JANS_NUM_SYMBOLS * 2];
    for (int i = 0; i < JANS_NUM_SYMBOLS; i++) {
        freq_buf[i*2] = (uint8_t)(table.freq[i] >> 8);
        freq_buf[i*2+1] = (uint8_t)(table.freq[i]);
    }
    int freq_size = JANS_NUM_SYMBOLS * 2;

    size_t total = 16 + freq_size + rans_size + resid_size;
    if (total > out_capacity) { arena_free(&arena); return -1; }

    uint8_t *op = out_buf;
    *op++ = (token_count>>24)&0xFF; *op++ = (token_count>>16)&0xFF;
    *op++ = (token_count>>8)&0xFF;  *op++ = token_count&0xFF;
    *op++ = (freq_size>>24)&0xFF;   *op++ = (freq_size>>16)&0xFF;
    *op++ = (freq_size>>8)&0xFF;    *op++ = freq_size&0xFF;
    *op++ = (rans_size>>24)&0xFF;   *op++ = (rans_size>>16)&0xFF;
    *op++ = (rans_size>>8)&0xFF;    *op++ = rans_size&0xFF;
    *op++ = (resid_size>>24)&0xFF;  *op++ = (resid_size>>16)&0xFF;
    *op++ = (resid_size>>8)&0xFF;   *op++ = resid_size&0xFF;
    memcpy(op, freq_buf, freq_size); op += freq_size;
    memcpy(op, rans_buf, rans_size); op += rans_size;
    memcpy(op, resid_buf, resid_size);

    arena_free(&arena);  /* Single free for all encode buffers */
#if defined(JANS_TIMING_DETAIL)
    _jans_out_ms += _jans_ms() - _t0;
#endif
    return (int)total;
}

#if defined(JANS_TIMING_DETAIL)
void jans_encode_band_x4_report_timing(void) {
    fprintf(stderr, "  jans: tokenize=%.1fms, rans=%.1fms, out=%.1fms (total over all bands)\n",
            _jans_tok_ms, _jans_rans_ms, _jans_out_ms);
    _jans_tok_ms = 0; _jans_rans_ms = 0; _jans_out_ms = 0;
}
#endif

/* ================================================================
   Inline-tokenize implementation. Mirrors jans_encode_band_x4's
   tokenize loop exactly so the produced tokens/bitbuf/freq are
   bit-identical to the split-pass implementation.
   ================================================================ */

/* Marker placed at byte 0 of a per-band blob to signal stripe-mode format.
   The legacy single-blob format starts with the token_count as a 32-bit
   value; real images never approach 0xFFFFFFFF tokens, so this is safe. */
#define JANS_STRIPE_MARKER 0xFFFFFFFFu

/* Deferred-rANS bookkeeping: one record per stripe, captured at the moment
   of a stripe boundary in Pass 1. Carries everything emit_blob needs to
   re-build that stripe's blob later (Pass 2): a snapshot of the freq table,
   pointers/lengths into the growing tokens[] and resid_buf[] arenas, and
   the row_count for the stripe header. */
typedef struct {
    size_t   token_off;     /* tokens[token_off .. token_off+token_count) */
    int      token_count;
    size_t   resid_off;     /* resid_buf[resid_off .. resid_off+resid_size) */
    size_t   resid_size;
    int      rows_in_stripe;
    uint16_t freq[JANS_NUM_SYMBOLS + 1];
} JANS_PENDING_STRIPE;

struct JANS_INLINE_STATE {
    /* Per-stripe state (or full-band if stripe_rows == 0) */
    uint16_t *tokens;
    int       token_count;
    size_t    token_cap;
    uint8_t  *resid_buf;
    size_t    resid_cap;
    BITBUF    bb;
    JANS_TABLE table;
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    int       profile_enabled;
    char      profile_label[16];
    uint64_t  profile_coeffs;
    uint64_t  profile_nonzero;
    uint64_t  profile_tokens;
    uint64_t  profile_resid_bits;
    uint32_t  profile_freq[JANS_NUM_SYMBOLS + 1];
    uint32_t  profile_max_symbol_freq;
    uint64_t  profile_overflow_symbols;
#endif
    /* Stripe-mode bookkeeping. stripe_rows == 0 means single-blob mode. */
    int       stripe_rows;
    int       rows_in_stripe;   /* rows accumulated since last flush */
    /* Accumulated stripe blobs (each preceded by row_count + size headers).
       Grows as flushes happen; emitted by jans_inline_finalize. */
    uint8_t  *stripe_acc;
    size_t    stripe_acc_pos;
    size_t    stripe_acc_cap;
    int       num_stripes;

    /* Persistent rANS scratch reused across every blob emit. Previously
       malloc'd inside jans_inline_emit_blob: stripe mode at q=3 / 50 MP =
       ~132 mallocs of a few MB per frame, all returned to libc and re-acquired
       next frame. */
    uint8_t  *rans_scratch;
    size_t    rans_scratch_cap;

    /* Deferred-rANS mode. When defer_rans != 0, jans_inline_flush_stripe
       snapshots the per-stripe state into pending[] and lets the tokens
       and resid arenas grow across stripes. jans_inline_finalize then
       walks pending[] and runs the rANS encode for each stripe in one
       shot (band-parallel via the existing Pass 2 thread). */
    int                  defer_rans;
    /* Token / resid arenas don't reset between stripes in defer mode. */
    size_t               tokens_base;  /* offset in tokens[] where current stripe begins */
    size_t               resid_base;   /* offset in resid_buf[] where current stripe begins */
    JANS_PENDING_STRIPE *pending;
    int                  pending_count;
    int                  pending_cap;
};

JANS_INLINE_STATE *jans_inline_create(size_t max_coeffs) {
    JANS_INLINE_STATE *s = (JANS_INLINE_STATE *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    init_run_lut();
    init_mag_lut(); init_sym_bits();
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    {
        const char *profile_env = getenv("JANS_INLINE_PROFILE");
        s->profile_enabled = (profile_env && profile_env[0] == '1') ? 1 : 0;
    }
#endif
    /* Worst-case sizing per stripe (legacy) or per full band (defer).
       In defer mode each coefficient can emit ~25 bits of resid; per token
       we need <= 4 bytes of bitbuf headroom. + slack for run-256 splitting
       producing extra tokens (~width/256 per all-zero row). */
    s->token_cap = max_coeffs + (max_coeffs / 32) + 4096;
    s->resid_cap = max_coeffs * 4 + 4096;
    s->tokens    = (uint16_t *)malloc(s->token_cap * sizeof(uint16_t));
    s->resid_buf = (uint8_t  *)malloc(s->resid_cap);
    if (!s->tokens || !s->resid_buf) { jans_inline_destroy(s); return NULL; }
    /* Pre-fault pages so the first frame doesn't pay a page-fault tax */
    memset(s->tokens, 0, s->token_cap * sizeof(uint16_t));
    memset(s->resid_buf, 0, s->resid_cap);
    /* Stripe accumulator allocated lazily in finalize / on first flush */
    s->stripe_acc = NULL;
    s->stripe_acc_cap = 0;
    return s;
}

void jans_inline_set_stripe_rows(JANS_INLINE_STATE *s, int stripe_rows) {
    if (!s) return;
    s->stripe_rows = stripe_rows;
}

void jans_inline_set_defer_rans(JANS_INLINE_STATE *s, int defer) {
    if (!s) return;
    s->defer_rans = defer ? 1 : 0;
}

void jans_inline_set_profile_label(JANS_INLINE_STATE *s, const char *label) {
    if (!s) return;
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    if (!label || !label[0]) {
        s->profile_label[0] = '\0';
        return;
    }
    snprintf(s->profile_label, sizeof(s->profile_label), "%s", label);
#else
    (void)label;
#endif
}

void jans_inline_reset(JANS_INLINE_STATE *s) {
    if (!s) return;
    s->token_count = 0;
    s->rows_in_stripe = 0;
    s->num_stripes = 0;
    s->stripe_acc_pos = 0;
    s->tokens_base = 0;
    s->resid_base = 0;
    s->pending_count = 0;
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    s->profile_coeffs = 0;
    s->profile_nonzero = 0;
    s->profile_tokens = 0;
    s->profile_resid_bits = 0;
    memset(s->profile_freq, 0, sizeof(s->profile_freq));
    s->profile_max_symbol_freq = 0;
    s->profile_overflow_symbols = 0;
#endif
    bitbuf_init(&s->bb, s->resid_buf, s->resid_cap);
    memset(&s->table, 0, sizeof(s->table));
}

/* Build a single-blob (legacy format) from explicit token/resid/freq inputs.
   Output is the same byte layout as jans_encode_band_x4 produces.
   Used both by jans_inline_finalize (single-blob mode), by the stripe
   flush helper, and by the deferred-rANS finalize path. Returns bytes
   written, or -1 on error.

   The caller supplies a JANS_TABLE workspace whose `freq[]` has been
   filled with the per-stripe symbol counts. emit_blob normalizes and
   builds the encode tables in-place, then runs rANS. */
static int jans_inline_emit_blob_ex(uint8_t *out_buf, size_t out_capacity,
                                    JANS_INLINE_STATE *s,
                                    JANS_TABLE *table,
                                    const uint16_t *tokens, int token_count,
                                    const uint8_t *resid, size_t resid_size)
{
    normalize_freq(table->freq, JANS_NUM_SYMBOLS);
    build_encode_tables(table, JANS_NUM_SYMBOLS);

    size_t rans_cap = (size_t)token_count * 4 + 4096;
    if (rans_cap > s->rans_scratch_cap) {
        uint8_t *nb = (uint8_t *)realloc(s->rans_scratch, rans_cap);
        if (!nb) return -1;
        s->rans_scratch = nb;
        s->rans_scratch_cap = rans_cap;
    }
    uint8_t *rans_buf = s->rans_scratch;
    uint8_t *rans_ptr = rans_buf;
    uint32_t states[JANS_INTERLEAVE];
    for (int j = 0; j < JANS_INTERLEAVE; j++) states[j] = RANS_BYTE_L;

    /* 4-way unrolled rANS encode (matches jans_encode_band_x4) — uses
       packed table.enc[sym] so each symbol fetch is one 8-byte load
       instead of 3 scattered loads from freq[]/cum_freq[]/rcp_freq[]. */
    const JANS_ENC_ENTRY *enc = table->enc;
    int i = token_count - 1;
    while (i >= 0 && (i & 3) != 3) {
        JANS_ENC_ENTRY e = enc[tokens[i]];
        rans_ptr = rans_enc_put(&states[i & 3], rans_ptr,
                     e.cum_freq, e.freq, e.rcp_freq);
        i--;
    }
    for (; i >= 3; i -= 4) {
        JANS_ENC_ENTRY e3 = enc[tokens[i]];
        JANS_ENC_ENTRY e2 = enc[tokens[i - 1]];
        JANS_ENC_ENTRY e1 = enc[tokens[i - 2]];
        JANS_ENC_ENTRY e0 = enc[tokens[i - 3]];
        rans_ptr = rans_enc_put(&states[3], rans_ptr,
                     e3.cum_freq, e3.freq, e3.rcp_freq);
        rans_ptr = rans_enc_put(&states[2], rans_ptr,
                     e2.cum_freq, e2.freq, e2.rcp_freq);
        rans_ptr = rans_enc_put(&states[1], rans_ptr,
                     e1.cum_freq, e1.freq, e1.rcp_freq);
        rans_ptr = rans_enc_put(&states[0], rans_ptr,
                     e0.cum_freq, e0.freq, e0.rcp_freq);
    }
    while (i >= 0) {
        JANS_ENC_ENTRY e = enc[tokens[i]];
        rans_ptr = rans_enc_put(&states[i & 3], rans_ptr,
                     e.cum_freq, e.freq, e.rcp_freq);
        i--;
    }
    for (int j = JANS_INTERLEAVE - 1; j >= 0; j--) {
        *rans_ptr++ = (uint8_t)(states[j] >> 0);
        *rans_ptr++ = (uint8_t)(states[j] >> 8);
        *rans_ptr++ = (uint8_t)(states[j] >> 16);
        *rans_ptr++ = (uint8_t)(states[j] >> 24);
    }
    size_t rans_size = rans_ptr - rans_buf;
    /* In-place byte reverse — rANS encodes backward, so the bitstream
       is currently in reverse order on disk. Vectorize 32 bytes per
       iter on NEON: load 16 from each end, vqtbl1q with reverse-index
       reverses each 16-byte vector in one cycle, then swap-store.
       Clang doesn't auto-vectorize this because the in-place swap looks
       aliased; we know it isn't (lo and hi never overlap inside the
       loop). */
    {
        size_t lo = 0, hi = rans_size;
#if defined(__ARM_NEON)
        static const uint8_t rev_idx_bytes[16] = {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0};
        const uint8x16_t rev_idx = vld1q_u8(rev_idx_bytes);
        while (lo + 32 <= hi) {
            uint8x16_t l = vld1q_u8(&rans_buf[lo]);
            uint8x16_t h = vld1q_u8(&rans_buf[hi - 16]);
            vst1q_u8(&rans_buf[lo],       vqtbl1q_u8(h, rev_idx));
            vst1q_u8(&rans_buf[hi - 16],  vqtbl1q_u8(l, rev_idx));
            lo += 16;
            hi -= 16;
        }
#endif
        while (lo + 1 < hi + 1 && lo < hi) {
            uint8_t t = rans_buf[lo];
            rans_buf[lo] = rans_buf[hi - 1];
            rans_buf[hi - 1] = t;
            lo++;
            hi--;
        }
    }

    uint8_t freq_buf[JANS_NUM_SYMBOLS * 2];
    for (int j = 0; j < JANS_NUM_SYMBOLS; j++) {
        freq_buf[j*2]   = (uint8_t)(table->freq[j] >> 8);
        freq_buf[j*2+1] = (uint8_t)(table->freq[j]);
    }
    int freq_size = JANS_NUM_SYMBOLS * 2;
    size_t total = 16 + freq_size + rans_size + resid_size;
    if (total > out_capacity) return -1;

    uint8_t *op = out_buf;
    int tc = token_count;
    *op++ = (tc>>24)&0xFF; *op++ = (tc>>16)&0xFF; *op++ = (tc>>8)&0xFF; *op++ = tc&0xFF;
    *op++ = (freq_size>>24)&0xFF; *op++ = (freq_size>>16)&0xFF;
    *op++ = (freq_size>>8)&0xFF;  *op++ = freq_size&0xFF;
    *op++ = (rans_size>>24)&0xFF; *op++ = (rans_size>>16)&0xFF;
    *op++ = (rans_size>>8)&0xFF;  *op++ = rans_size&0xFF;
    *op++ = (resid_size>>24)&0xFF; *op++ = (resid_size>>16)&0xFF;
    *op++ = (resid_size>>8)&0xFF;  *op++ = resid_size&0xFF;
    memcpy(op, freq_buf, freq_size); op += freq_size;
    memcpy(op, rans_buf, rans_size); op += rans_size;
    memcpy(op, resid, resid_size);

    return (int)total;
}

/* Legacy entry: builds blob from s->tokens / s->bb / s->table. Wrapper around
   the parameterized emit. */
static int jans_inline_emit_blob(uint8_t *out_buf, size_t out_capacity,
                                 JANS_INLINE_STATE *s)
{
    size_t resid_size = bitbuf_size(&s->bb);
    return jans_inline_emit_blob_ex(out_buf, out_capacity, s, &s->table,
                                    s->tokens, s->token_count,
                                    s->resid_buf, resid_size);
}

/* Snapshot the current stripe's tokens/resid/freq into pending[], then
   advance the stripe baseline so subsequent rows keep growing the same
   tokens[] and resid_buf[] arenas. Called from flush_stripe when
   defer_rans is enabled.

   We need to capture resid_size BEFORE re-initing the bitbuf for the next
   stripe; that requires flushing the bitbuf's partial-byte tail so emit
   later sees the same byte boundary as the immediate path would have. */
static int jans_inline_defer_snapshot(JANS_INLINE_STATE *s) {
    if (s->pending_count >= s->pending_cap) {
        int new_cap = s->pending_cap ? s->pending_cap * 2 : 16;
        JANS_PENDING_STRIPE *nb = (JANS_PENDING_STRIPE *)realloc(
            s->pending, (size_t)new_cap * sizeof(JANS_PENDING_STRIPE));
        if (!nb) return -1;
        s->pending = nb;
        s->pending_cap = new_cap;
    }

    /* Flush trailing partial byte into resid_buf so the stripe is byte-aligned.
       bitbuf_size returns byte_pos relative to bb->buf (which has been slid
       forward to resid_buf + resid_base on previous snapshots), so the
       per-stripe size is bytes_in_this_bb, and the absolute end in the
       resid arena is resid_base + bytes_in_this_bb. */
    size_t stripe_resid_size = bitbuf_size(&s->bb);
    size_t resid_end_abs = s->resid_base + stripe_resid_size;

    JANS_PENDING_STRIPE *p = &s->pending[s->pending_count++];
    p->token_off       = s->tokens_base;
    p->token_count     = s->token_count;
    p->resid_off       = s->resid_base;
    p->resid_size      = stripe_resid_size;
    p->rows_in_stripe  = s->rows_in_stripe;
    memcpy(p->freq, s->table.freq, sizeof(p->freq));

#if defined(JANS_INLINE_PROFILE_RUNTIME)
    if (__builtin_expect(s->profile_enabled != 0, 0)) {
        uint32_t max_freq = 0;
        uint32_t overflow_symbols = 0;
        for (int i = 0; i < JANS_NUM_SYMBOLS + 1; i++) {
            uint32_t f = s->profile_freq[i];
            if (f > max_freq) max_freq = f;
            if (f > 0xFFFFu) overflow_symbols++;
            s->profile_freq[i] = 0;
        }
        if (max_freq > s->profile_max_symbol_freq) {
            s->profile_max_symbol_freq = max_freq;
        }
        s->profile_overflow_symbols += overflow_symbols;
    }
#endif

    /* Advance baselines; next stripe writes after these in the same arenas. */
    s->tokens_base = s->tokens_base + (size_t)s->token_count;
    s->resid_base  = resid_end_abs;

    /* Reset per-stripe counters but keep growing the arenas. Slide the
       bitbuf window forward so the next stripe's writes land contiguously
       after this stripe's in the same resid_buf. */
    s->token_count    = 0;
    s->rows_in_stripe = 0;
    if (resid_end_abs <= s->resid_cap) {
        bitbuf_init(&s->bb, s->resid_buf + resid_end_abs,
                    s->resid_cap - resid_end_abs);
    } else {
        /* Capacity exhausted — this should not happen with proper sizing. */
        bitbuf_init(&s->bb, s->resid_buf + s->resid_cap, 0);
    }
    memset(&s->table, 0, sizeof(s->table));
    return 0;
}

#if defined(JANS_INLINE_PROFILE_RUNTIME)
static void jans_inline_profile_finish_symbol_counts(JANS_INLINE_STATE *s) {
    if (!s || !s->profile_enabled) return;
    uint32_t max_freq = 0;
    uint32_t overflow_symbols = 0;
    for (int i = 0; i < JANS_NUM_SYMBOLS + 1; i++) {
        uint32_t f = s->profile_freq[i];
        if (f > max_freq) max_freq = f;
        if (f > 0xFFFFu) overflow_symbols++;
        s->profile_freq[i] = 0;
    }
    if (max_freq > s->profile_max_symbol_freq) {
        s->profile_max_symbol_freq = max_freq;
    }
    s->profile_overflow_symbols += overflow_symbols;
}
#endif

/* Encode the currently accumulated tokens as a stripe blob; append to the
   stripe accumulator with row_count + size headers. Reset per-stripe state
   for the next stripe.

   In deferred-rANS mode this just snapshots into pending[] — the rANS
   work moves to jans_inline_finalize. */
static int jans_inline_flush_stripe(JANS_INLINE_STATE *s) {
    if (s->token_count == 0 && s->rows_in_stripe == 0) return 0;

    if (s->defer_rans) {
        return jans_inline_defer_snapshot(s);
    }

    /* Worst case: stripe blob is roughly 16 + 256 + (tokens*4) + resid bytes. */
    size_t blob_max = 16 + JANS_NUM_SYMBOLS * 2
                    + (size_t)s->token_count * 4 + 4096
                    + s->resid_cap + 64;
    /* Stripe header is 8 bytes (row_count + size). */
    size_t need = s->stripe_acc_pos + 8 + blob_max;
    if (need > s->stripe_acc_cap) {
        size_t new_cap = s->stripe_acc_cap ? s->stripe_acc_cap * 2 : 256 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t *new_buf = (uint8_t *)realloc(s->stripe_acc, new_cap);
        if (!new_buf) return -1;
        s->stripe_acc = new_buf;
        s->stripe_acc_cap = new_cap;
    }

    /* Reserve 8 bytes for [row_count][blob_size], then encode the blob. */
    uint8_t *hdr = s->stripe_acc + s->stripe_acc_pos;
    uint8_t *blob_dst = hdr + 8;
    int blob_size = jans_inline_emit_blob(blob_dst, blob_max, s);
    if (blob_size < 0) return -1;

#if defined(JANS_INLINE_PROFILE_RUNTIME)
    jans_inline_profile_finish_symbol_counts(s);
#endif

    int rc = s->rows_in_stripe;
    hdr[0] = (rc>>24)&0xFF; hdr[1] = (rc>>16)&0xFF;
    hdr[2] = (rc>>8)&0xFF;  hdr[3] = rc&0xFF;
    hdr[4] = (blob_size>>24)&0xFF; hdr[5] = (blob_size>>16)&0xFF;
    hdr[6] = (blob_size>>8)&0xFF;  hdr[7] = blob_size&0xFF;

    s->stripe_acc_pos += 8 + blob_size;
    s->num_stripes++;

    /* Reset per-stripe accumulators for the next stripe */
    s->token_count = 0;
    s->rows_in_stripe = 0;
    bitbuf_init(&s->bb, s->resid_buf, s->resid_cap);
    memset(&s->table, 0, sizeof(s->table));
    return 0;
}

void jans_inline_row(JANS_INLINE_STATE *s, const int32_t *row, int width) {
    if (!s) return;
    /* In defer mode tokens grow forward across stripes; offset by tokens_base
       so this stripe's writes land contiguously after the prior stripe's. */
    uint16_t *tokens = s->tokens + (s->defer_rans ? s->tokens_base : 0);
    int token_count = s->token_count;
    uint16_t *tokp = tokens + token_count;
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    const int profile = s->profile_enabled;
    uint64_t profile_nonzero = 0;
    uint64_t profile_tokens = 0;
    uint64_t profile_resid_bits = 0;
#endif
    int run = 0;
    /* Hoist freq + bb pointer out of the struct access. */
    uint16_t *freq = s->table.freq;
    BITBUF *bb = &s->bb;

    /* Pull bitbuf fields into locals across the whole row. Otherwise every
       bitbuf_write call must reload accum/accum_bits from memory (forced
       by the bb pointer) and write them back, creating a serial dep chain
       of ~5 cycles per nonzero. With locals the compiler keeps these in
       registers across all calls in this row, breaking the chain.
       Writeback at end of row preserves correctness across rows. */
    uint64_t bbacc = bb->accum;
    int bbbits = bb->accum_bits;
    size_t bbpos = bb->byte_pos;
    uint8_t * const bbbuf = bb->buf;
    const size_t bbcap = bb->capacity;

#define BB_WRITE_FAST(val32, nbits) do {                                   \
        uint32_t _v = (val32);                                             \
        int _b = (nbits);                                                  \
        PROFILE_RESID_BITS(_b);                                            \
        /* Callers construct residual payloads that already fit _b bits. */ \
        bbacc |= ((uint64_t)_v) << bbbits;                                 \
        bbbits += _b;                                                      \
        if (bbbits >= 32 && bbpos + 4 <= bbcap) {                          \
            uint32_t _w = (uint32_t)bbacc;                                 \
            memcpy(bbbuf + bbpos, &_w, 4);                                 \
            bbpos += 4;                                                    \
            bbacc >>= 32;                                                  \
            bbbits -= 32;                                                  \
        }                                                                  \
    } while (0)

#if defined(JANS_INLINE_PROFILE_RUNTIME)
#define PROFILE_RESID_BITS(nbits) do {                                     \
        if (__builtin_expect(profile != 0, 0)) profile_resid_bits += (uint64_t)_b; \
    } while (0)
#define PROFILE_NONZERO() do { if (__builtin_expect(profile != 0, 0)) profile_nonzero++; } while (0)
#define PROFILE_TOKEN() do { if (__builtin_expect(profile != 0, 0)) profile_tokens++; } while (0)
#define PROFILE_FREQ(sym_) do { if (__builtin_expect(profile != 0, 0)) s->profile_freq[(sym_)]++; } while (0)
#else
#define PROFILE_RESID_BITS(nbits) do { (void)(nbits); } while (0)
#define PROFILE_NONZERO() do { } while (0)
#define PROFILE_TOKEN() do { } while (0)
#define PROFILE_FREQ(sym_) do { } while (0)
#endif

#define PROCESS_NONZERO_VALUE(val_) do {                                    \
        int32_t nz_val = (val_);                                             \
        PROFILE_NONZERO();                                                   \
        int32_t mag = (nz_val < 0) ? -nz_val : nz_val;                       \
        while (run >= 256) {                                                 \
            int rr; int rc = run_to_class(255, &rr);                         \
            int sym = rc * JANS_MAG_CLASSES + 0;                             \
            JANS_FREQ_INC(freq[sym]);                                        \
            PROFILE_FREQ(sym);                                                \
            *tokp++ = (uint16_t)sym;                                         \
            PROFILE_TOKEN();                                                 \
            BB_WRITE_FAST(rr, run_class_bits[rc]);                           \
            run -= 255;                                                      \
        }                                                                    \
        int run_resid, mag_resid;                                            \
        int rc = run_to_class(run, &run_resid);                              \
        int mc = mag_to_class(mag, &mag_resid);                              \
        int sym = rc * JANS_MAG_CLASSES + mc;                                \
        JANS_FREQ_INC(freq[sym]);                                            \
        PROFILE_FREQ(sym);                                                    \
        *tokp++ = (uint16_t)sym;                                             \
        PROFILE_TOKEN();                                                     \
        int rb = sym_run_bits[sym];                                          \
        int tb = sym_total_bits[sym];                                        \
        int mb = mag_class_bits[mc];                                         \
        uint32_t merged = (uint32_t)run_resid;                               \
        merged |= ((uint32_t)mag_resid << rb);                               \
        merged |= ((nz_val < 0) ? 1u : 0u) << (rb + mb);                     \
        BB_WRITE_FAST(merged, tb);                                           \
        run = 0;                                                             \
    } while (0)
#define VALUE_IS_ZERO(val_) ((val_) == 0)

    int col = 0;
#if defined(__ARM_NEON)
    {
#if defined(__aarch64__)
#define VEC4_ALL_ZERO(v_) (vmaxvq_u32(vreinterpretq_u32_s32(v_)) == 0)
#else
        const int32x4_t vzero = vdupq_n_s32(0);
#define VEC4_ALL_ZERO(v_) ({                                                \
            uint32x4_t _nonzero = vmvnq_u32(vceqq_s32((v_), vzero));        \
            uint64x2_t _nz64 = vreinterpretq_u64_u32(_nonzero);             \
            (vgetq_lane_u64(_nz64, 0) | vgetq_lane_u64(_nz64, 1)) == 0;      \
        })
#endif
#define PROCESS_VEC4(base_, vec_) do {                                      \
                if (VEC4_ALL_ZERO(vec_)) {                                  \
                    run += 4;                                               \
                } else {                                                    \
                    int32_t _v0 = row[(base_) + 0];                         \
                    int32_t _v1 = row[(base_) + 1];                         \
                    int32_t _v2 = row[(base_) + 2];                         \
                    int32_t _v3 = row[(base_) + 3];                         \
                    if (VALUE_IS_ZERO(_v0)) run++; else PROCESS_NONZERO_VALUE(_v0); \
                    if (VALUE_IS_ZERO(_v1)) run++; else PROCESS_NONZERO_VALUE(_v1); \
                    if (VALUE_IS_ZERO(_v2)) run++; else PROCESS_NONZERO_VALUE(_v2); \
                    if (VALUE_IS_ZERO(_v3)) run++; else PROCESS_NONZERO_VALUE(_v3); \
                }                                                           \
            } while (0)
        int width_m16 = (width / 16) * 16;
        for (; col < width_m16; ) {
            int32x4_t v0 = vld1q_s32(row + col + 0);
            int32x4_t v1 = vld1q_s32(row + col + 4);
            int32x4_t v2 = vld1q_s32(row + col + 8);
            int32x4_t v3 = vld1q_s32(row + col + 12);
            if (VEC4_ALL_ZERO(v0) && VEC4_ALL_ZERO(v1) &&
                VEC4_ALL_ZERO(v2) && VEC4_ALL_ZERO(v3)) {
                run += 16;
                col += 16;
                continue;
            }
            PROCESS_VEC4(col + 0, v0);
            PROCESS_VEC4(col + 4, v1);
            PROCESS_VEC4(col + 8, v2);
            PROCESS_VEC4(col + 12, v3);
            col += 16;
        }
        int width_m4 = (width / 4) * 4;
        for (; col < width_m4; col += 4) {
            int32x4_t v = vld1q_s32(row + col);
            if (VEC4_ALL_ZERO(v)) {
                run += 4;
                continue;
            }
            int32_t v0 = row[col + 0];
            int32_t v1 = row[col + 1];
            int32_t v2 = row[col + 2];
            int32_t v3 = row[col + 3];
            if (VALUE_IS_ZERO(v0)) run++; else PROCESS_NONZERO_VALUE(v0);
            if (VALUE_IS_ZERO(v1)) run++; else PROCESS_NONZERO_VALUE(v1);
            if (VALUE_IS_ZERO(v2)) run++; else PROCESS_NONZERO_VALUE(v2);
            if (VALUE_IS_ZERO(v3)) run++; else PROCESS_NONZERO_VALUE(v3);
        }
#undef PROCESS_VEC4
#undef VEC4_ALL_ZERO
    }
#endif
    for (; col < width; col++) {
        int32_t val = row[col];
        if (VALUE_IS_ZERO(val)) { run++; continue; }
        PROCESS_NONZERO_VALUE(val);
    }
#undef VALUE_IS_ZERO
#undef PROCESS_NONZERO_VALUE
    /* End-of-row trailing-zero flush — matches jans_encode_band_x4 boundary */
    while (run > 0) {
        int actual = (run > 255) ? 255 : run;
        int rr; int rc = run_to_class(actual, &rr);
        int sym = rc * JANS_MAG_CLASSES + 0;
        JANS_FREQ_INC(freq[sym]);
        PROFILE_FREQ(sym);
        *tokp++ = (uint16_t)sym;
        PROFILE_TOKEN();
        BB_WRITE_FAST(rr, run_class_bits[rc]);
        run -= actual;
    }
    /* Writeback bitbuf locals to struct so emit_blob sees them. */
    bb->accum = bbacc;
    bb->accum_bits = bbbits;
    bb->byte_pos = bbpos;
#undef BB_WRITE_FAST

    s->token_count = (int)(tokp - tokens);
    s->rows_in_stripe++;
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    if (__builtin_expect(profile != 0, 0)) {
        s->profile_coeffs += (uint64_t)width;
        s->profile_nonzero += profile_nonzero;
        s->profile_tokens += profile_tokens;
        s->profile_resid_bits += profile_resid_bits;
    }
#endif
#undef PROFILE_RESID_BITS
#undef PROFILE_NONZERO
#undef PROFILE_TOKEN
#undef PROFILE_FREQ

    /* Stripe-mode auto-flush */
    if (s->stripe_rows > 0 && s->rows_in_stripe >= s->stripe_rows) {
        (void)jans_inline_flush_stripe(s);
    }
}

#if defined(JANS_INLINE_PROFILE_RUNTIME)
static void jans_inline_maybe_print_profile(const JANS_INLINE_STATE *s, size_t bytes) {
    if (!s || !s->profile_enabled) return;
    uint64_t zeros = 0;
    if (s->profile_coeffs >= s->profile_nonzero) {
        zeros = s->profile_coeffs - s->profile_nonzero;
    }
    fprintf(stderr,
            "# jans_inline_profile label=%s coeffs=%llu zero=%llu nonzero=%llu tokens=%llu resid_bits=%llu max_symbol_freq=%u overflow_symbols=%llu stripe_rows=%d defer=%d bytes=%zu\n",
            s->profile_label[0] ? s->profile_label : "unlabeled",
            (unsigned long long)s->profile_coeffs,
            (unsigned long long)zeros,
            (unsigned long long)s->profile_nonzero,
            (unsigned long long)s->profile_tokens,
            (unsigned long long)s->profile_resid_bits,
            s->profile_max_symbol_freq,
            (unsigned long long)s->profile_overflow_symbols,
            s->stripe_rows,
            s->defer_rans,
            bytes);
}
#endif

int jans_inline_finalize(uint8_t *out_buf, size_t out_capacity, JANS_INLINE_STATE *s) {
    if (!s || !out_buf) return -1;

    if (s->stripe_rows <= 0) {
        /* Legacy single-blob mode: emit one blob, identical to old format */
        int written = jans_inline_emit_blob(out_buf, out_capacity, s);
#if defined(JANS_INLINE_PROFILE_RUNTIME)
        if (written >= 0) {
            jans_inline_profile_finish_symbol_counts(s);
            jans_inline_maybe_print_profile(s, (size_t)written);
        }
#endif
        return written;
    }

    /* Stripe mode: flush any pending stripe, then emit the framing header
       followed by all accumulated stripe-blob bytes. */
    if (s->rows_in_stripe > 0) {
        if (jans_inline_flush_stripe(s) != 0) return -1;
    }

    /* Deferred-rANS mode: run the rANS encode for each pending stripe here
       (Pass 2), then emit the stripe container. This is the heavy lifting
       moved out of Pass 1's critical path. */
    if (s->defer_rans) {
        /* Emit directly into out_buf to avoid one memcpy through stripe_acc. */
        if (16 > out_capacity) return -1;
        uint8_t *op = out_buf;
        op[0] = 0xFF; op[1] = 0xFF; op[2] = 0xFF; op[3] = 0xFF; op += 4;
        int ns = s->pending_count;
        op[0] = (ns>>24)&0xFF; op[1] = (ns>>16)&0xFF;
        op[2] = (ns>>8)&0xFF;  op[3] = ns&0xFF; op += 4;
        memset(op, 0, 8); op += 8;

        size_t pos = 16;
        JANS_TABLE tbl_ws;
        for (int k = 0; k < s->pending_count; k++) {
            const JANS_PENDING_STRIPE *p = &s->pending[k];
            if (pos + 8 > out_capacity) return -1;
            uint8_t *hdr = out_buf + pos;
            uint8_t *blob_dst = hdr + 8;
            size_t blob_cap = (out_capacity > pos + 8) ? (out_capacity - pos - 8) : 0;

            memset(&tbl_ws, 0, sizeof(tbl_ws));
            memcpy(tbl_ws.freq, p->freq, sizeof(tbl_ws.freq));

            int blob_size = jans_inline_emit_blob_ex(
                blob_dst, blob_cap, s, &tbl_ws,
                s->tokens + p->token_off, p->token_count,
                s->resid_buf + p->resid_off, p->resid_size);
            if (blob_size < 0) return -1;

            int rc = p->rows_in_stripe;
            hdr[0] = (rc>>24)&0xFF; hdr[1] = (rc>>16)&0xFF;
            hdr[2] = (rc>>8)&0xFF;  hdr[3] = rc&0xFF;
            hdr[4] = (blob_size>>24)&0xFF; hdr[5] = (blob_size>>16)&0xFF;
            hdr[6] = (blob_size>>8)&0xFF;  hdr[7] = blob_size&0xFF;
            pos += 8 + (size_t)blob_size;
        }
#if defined(JANS_INLINE_PROFILE_RUNTIME)
        jans_inline_maybe_print_profile(s, pos);
#endif
        return (int)pos;
    }

    size_t total = 16 + s->stripe_acc_pos;
    if (total > out_capacity) return -1;

    uint8_t *op = out_buf;
    /* Stripe marker (4 bytes) */
    op[0] = 0xFF; op[1] = 0xFF; op[2] = 0xFF; op[3] = 0xFF; op += 4;
    /* num_stripes (4 bytes) */
    int ns = s->num_stripes;
    op[0] = (ns>>24)&0xFF; op[1] = (ns>>16)&0xFF;
    op[2] = (ns>>8)&0xFF;  op[3] = ns&0xFF; op += 4;
    /* Reserved (8 bytes) */
    memset(op, 0, 8); op += 8;

    if (s->stripe_acc_pos > 0) {
        memcpy(op, s->stripe_acc, s->stripe_acc_pos);
    }
#if defined(JANS_INLINE_PROFILE_RUNTIME)
    jans_inline_maybe_print_profile(s, total);
#endif
    return (int)total;
}

void jans_inline_destroy(JANS_INLINE_STATE *s) {
    if (!s) return;
    if (s->tokens)        free(s->tokens);
    if (s->resid_buf)     free(s->resid_buf);
    if (s->stripe_acc)    free(s->stripe_acc);
    if (s->rans_scratch)  free(s->rans_scratch);
    if (s->pending)       free(s->pending);
    free(s);
}

/* ================================================================
   Two-pass x4 decode: separates rANS token decoding from output scatter.

   Pass 1 does pure rANS + bitbuf work into a sequential buffer.
   Pass 2 scatters to the 2D output. Separating these improves:
   - Cache behavior in pass 1 (sequential writes, no pitch-gap skips)
   - Enables bulk memset for zero runs in pass 2
   - On weak ARM cores (Cortex-A53/A78), avoids the scatter-in-loop
     pattern that causes TLB and store-buffer stalls.

   SoA layout: separate runs[] and values[] arrays instead of AoS.
   This gives tighter packing in pass 1 (writes to two streams instead
   of scattered struct fields) and lets pass 2 read runs[] without
   pulling in values[] cache lines for run-only tokens.
   ================================================================ */

/* Forward declaration: legacy single-blob decoder, used by both the public
   entry point (when no stripe marker is present) and by the stripe loop. */
static int jans_decode_band_x4_single(const uint8_t *in_buf, size_t in_size,
                                       int32_t *data, int width, int height, int pitch);

int jans_decode_band_x4(const uint8_t *in_buf, size_t in_size,
                        int32_t *data, int width, int height, int pitch) {
    if (in_size < 16) return -1;

    /* Detect stripe-mode header: 4 bytes of 0xFF where the legacy format
       would have token_count (always a small unsigned). */
    if (in_buf[0] == 0xFF && in_buf[1] == 0xFF &&
        in_buf[2] == 0xFF && in_buf[3] == 0xFF)
    {
        const uint8_t *p = in_buf + 4;
        int num_stripes = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
        p += 8;  /* reserved */
        if (num_stripes < 0 || num_stripes > height) return -1;

        size_t pos = (size_t)(p - in_buf);
        int row_offset = 0;
        for (int k = 0; k < num_stripes; k++) {
            if (pos + 8 > in_size) return -1;
            const uint8_t *sp = in_buf + pos;
            int stripe_rows = (sp[0]<<24)|(sp[1]<<16)|(sp[2]<<8)|sp[3];
            int stripe_size = (sp[4]<<24)|(sp[5]<<16)|(sp[6]<<8)|sp[7];
            if (stripe_rows < 0 || stripe_size < 0) return -1;
            if (pos + 8 + (size_t)stripe_size > in_size) return -1;
            if (row_offset + stripe_rows > height) return -1;

            int32_t *stripe_data = data + (size_t)row_offset * (pitch / sizeof(int32_t));
            int rc = jans_decode_band_x4_single(in_buf + pos + 8, stripe_size,
                                                stripe_data, width,
                                                stripe_rows, pitch);
            if (rc != 0) return rc;

            row_offset += stripe_rows;
            pos += 8 + (size_t)stripe_size;
        }
        /* Any rows past the last stripe should already be zero (the band was
           allocated zero-initialized). */
        return 0;
    }

    /* Legacy single-blob path */
    return jans_decode_band_x4_single(in_buf, in_size, data, width, height, pitch);
}

static int jans_decode_band_x4_single(const uint8_t *in_buf, size_t in_size,
                                       int32_t *data, int width, int height, int pitch) {
    if (in_size < 16) return -1;
    int pitch_elems = pitch / sizeof(int32_t);

    const uint8_t *p = in_buf;
    int token_count = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int freq_size   = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int rans_size   = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int resid_size  = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;

    if (token_count < 0 || freq_size < 0 || rans_size < (int)(4*JANS_INTERLEAVE) || resid_size < 0) return -1;
    if ((size_t)16 + (size_t)freq_size + (size_t)rans_size + (size_t)resid_size > in_size) return -1;
    if (freq_size < JANS_NUM_SYMBOLS * 2) return -1;

    const uint8_t *freq_data = p; p += freq_size;
    JANS_TABLE table;
    memset(&table, 0, sizeof(table));
    for (int i = 0; i < JANS_NUM_SYMBOLS && i*2+1 < freq_size; i++)
        table.freq[i] = ((uint16_t)freq_data[i*2] << 8) | freq_data[i*2+1];
    normalize_freq(table.freq, JANS_NUM_SYMBOLS);
    build_tables(&table, JANS_NUM_SYMBOLS);

    const uint8_t *rans_data = p; p += rans_size;
    const uint8_t *rans_end = rans_data + rans_size;
    const uint8_t *resid_data = p;

    /* Initialize 4 states */
    uint32_t states[JANS_INTERLEAVE];
    const uint8_t *rptr = rans_data;
    for (int s = 0; s < JANS_INTERLEAVE; s++) {
        states[s] = ((uint32_t)rptr[0]<<24) | ((uint32_t)rptr[1]<<16) |
                    ((uint32_t)rptr[2]<<8)  | (uint32_t)rptr[3];
        rptr += 4;
    }

    /* SoA intermediate buffers: separate runs[] and values[] arrays.
       Use thread-local reusable buffer to avoid per-band malloc overhead.
       For Z8 45MP: 36 bands × ~5MB each = 180MB of malloc/free eliminated. */
    size_t runs_bytes = ((size_t)token_count * sizeof(uint16_t) + 7) & ~(size_t)7;
    size_t values_bytes = (size_t)token_count * sizeof(int32_t);
    size_t alloc_size = runs_bytes + values_bytes;

    static _Thread_local uint8_t *tls_buf = NULL;
    static _Thread_local size_t tls_size = 0;
    uint8_t *alloc_buf;
    int alloc_is_tls = 0;

    int no_tls_cache = (getenv("JANS_DECODE_NO_TLS_CACHE") &&
                        getenv("JANS_DECODE_NO_TLS_CACHE")[0] == '1');

    if (!no_tls_cache && alloc_size <= tls_size && tls_buf) {
        alloc_buf = tls_buf;
        alloc_is_tls = 1;
    } else {
        alloc_buf = (uint8_t *)malloc(alloc_size);
        if (!alloc_buf) return -1;
        if (!no_tls_cache) {
            /* Update TLS cache for future reuse */
            if (tls_buf) free(tls_buf);
            tls_buf = alloc_buf;
            tls_size = alloc_size;
            alloc_is_tls = 1;
        }
    }

    uint16_t *runs = (uint16_t *)alloc_buf;
    int32_t *values = (int32_t *)(alloc_buf + runs_bytes);

    #define JANS_DECODE_RETURN(rc) do { \
        if (!alloc_is_tls) free(alloc_buf); \
        return (rc); \
    } while (0)

    /* ============================================================
       PASS 1: Decode all rANS tokens into flat SoA arrays.
       Pure sequential memory access — no output scatter, no row/col tracking.
       This is the hot loop and should be very cache-friendly.
       ============================================================ */

    size_t resid_byte = 0;
    int resid_bit = 0;
    int pair_count = 0;
    int t = 0;

    /* Main decode loop with 4-way unrolled state update + branch-free renorm. */
    while (t + JANS_INTERLEAVE <= token_count)
    {
        const JANS_DECODE_INFO *infos[4];
        uint32_t slots[4];

        slots[0] = states[0] & (JANS_TABLE_SIZE - 1);
        slots[1] = states[1] & (JANS_TABLE_SIZE - 1);
        slots[2] = states[2] & (JANS_TABLE_SIZE - 1);
        slots[3] = states[3] & (JANS_TABLE_SIZE - 1);
        infos[0] = &table.decode_info[slots[0]];
        infos[1] = &table.decode_info[slots[1]];
        infos[2] = &table.decode_info[slots[2]];
        infos[3] = &table.decode_info[slots[3]];

        states[0] = infos[0]->freq * (states[0] >> JANS_TABLE_BITS) + slots[0] - infos[0]->cum_freq;
        states[1] = infos[1]->freq * (states[1] >> JANS_TABLE_BITS) + slots[1] - infos[1]->cum_freq;
        states[2] = infos[2]->freq * (states[2] >> JANS_TABLE_BITS) + slots[2] - infos[2]->cum_freq;
        states[3] = infos[3]->freq * (states[3] >> JANS_TABLE_BITS) + slots[3] - infos[3]->cum_freq;

        /* Branch-free renorm with 2-byte fast path. After encode-side
           normalization, each state needs 0, 1 or 2 bytes to renormalize
           (3+ is mathematically possible but ~ rounding error rare).
           Replacing the per-state while-loop with a fixed-depth ladder
           removes the branch-prediction noise across 4 states. */
        #define DEC_RENORM(s) do { \
            if (__builtin_expect((s) < RANS_BYTE_L, 0)) { \
                (s) = ((s) << 8) | *rptr++; \
                if (__builtin_expect((s) < RANS_BYTE_L, 0)) { \
                    (s) = ((s) << 8) | *rptr++; \
                } \
            } \
        } while (0)
        DEC_RENORM(states[0]);
        DEC_RENORM(states[1]);
        DEC_RENORM(states[2]);
        DEC_RENORM(states[3]);
        #undef DEC_RENORM

        /* Bounds check post-hoc — typically not crossed; one branch per
           4-token batch instead of one per state. */
        if (__builtin_expect(rptr > rans_end, 0)) JANS_DECODE_RETURN(-1);

        __builtin_prefetch(&table.decode_info[states[0] & (JANS_TABLE_SIZE - 1)], 0, 3);
        __builtin_prefetch(&table.decode_info[states[1] & (JANS_TABLE_SIZE - 1)], 0, 3);
        __builtin_prefetch(&table.decode_info[states[2] & (JANS_TABLE_SIZE - 1)], 0, 3);
        __builtin_prefetch(&table.decode_info[states[3] & (JANS_TABLE_SIZE - 1)], 0, 3);

        /* Batch residual extraction: single 64-bit load for all 4 tokens' residual bits.
           Max total_bits per token is ~10, so 4 tokens ≤ 40 bits. With resid_bit offset
           up to 7, we need up to 47 bits — always fits in a 64-bit window. */
        {
            uint64_t resid_word = 0;
            if (__builtin_expect(resid_byte + 7 < resid_size, 1)) {
                memcpy(&resid_word, resid_data + resid_byte, 8);
                resid_word >>= resid_bit;
            } else {
                /* Near end of buffer: fall back to per-token reads */
                for (int s = 0; s < 4; s++) {
                    const JANS_DECODE_INFO *di = infos[s];
                    int idx = pair_count++;
                    uint32_t all_bits = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit, di->total_bits);
                    runs[idx] = (uint16_t)(di->run_min + (all_bits & ((1u << di->run_bits) - 1)));
                    all_bits >>= di->run_bits;
                    if (di->has_value) {
                        int mag = di->mag_min + (all_bits & ((1u << di->mag_bits) - 1));
                        all_bits >>= di->mag_bits;
                        values[idx] = (all_bits & 1) ? -mag : mag;
                    } else { values[idx] = 0; }
                }
                t += JANS_INTERLEAVE;
                continue;
            }

            /* Explicitly unrolled 4-way residual extraction. Per-state work
               is independent (different di entries → no RAW deps) so the
               CPU pipelines all four side-by-side. The per-token `has_value`
               branch is replaced with branchless arithmetic. */
            #define EXTRACT_ONE(SI) do {                                       \
                const JANS_DECODE_INFO *_di = infos[SI];                       \
                int _idx = pair_count + (SI);                                  \
                int _tb = _di->total_bits;                                     \
                uint32_t _mask = (uint32_t)((1u << _tb) - 1);                  \
                uint32_t _all = (uint32_t)(resid_word >> consumed) & _mask;    \
                consumed += _tb;                                               \
                int _rb = _di->run_bits;                                       \
                runs[_idx] = (uint16_t)(_di->run_min + (_all & ((1u << _rb) - 1))); \
                _all >>= _rb;                                                  \
                int _mb = _di->mag_bits;                                       \
                int _mag = _di->mag_min + (int)(_all & ((1u << _mb) - 1));     \
                _all >>= _mb;                                                  \
                int _signed = (_all & 1) ? -_mag : _mag;                       \
                /* has_value selects between signed_mag and 0 — branchless */  \
                values[_idx] = _di->has_value ? _signed : 0;                   \
            } while (0)

            int consumed = 0;
            EXTRACT_ONE(0);
            EXTRACT_ONE(1);
            EXTRACT_ONE(2);
            EXTRACT_ONE(3);
            #undef EXTRACT_ONE
            pair_count += 4;

            int new_bit = resid_bit + consumed;
            resid_byte += new_bit >> 3;
            resid_bit = new_bit & 7;
        }
        t += JANS_INTERLEAVE;
    }

    /* Handle remaining tokens (< 4) */
    for (; t < token_count; t++) {
        int s = t & (JANS_INTERLEAVE - 1);
        uint32_t slot = states[s] & (JANS_TABLE_SIZE - 1);
        const JANS_DECODE_INFO *di = &table.decode_info[slot];
        states[s] = di->freq * (states[s] >> JANS_TABLE_BITS) + slot - di->cum_freq;
        while (states[s] < RANS_BYTE_L) {
            if (rptr >= rans_end) { JANS_DECODE_RETURN(-1); }
            states[s] = (states[s] << 8) | *rptr++;
        }

        int idx = pair_count++;
        uint32_t all_bits = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit,
                                        di->total_bits);
        runs[idx] = (uint16_t)(di->run_min + (all_bits & ((1u << di->run_bits) - 1)));
        all_bits >>= di->run_bits;
        if (di->has_value) {
            int mag = di->mag_min + (all_bits & ((1u << di->mag_bits) - 1));
            all_bits >>= di->mag_bits;
            values[idx] = (all_bits & 1) ? -mag : mag;
        } else {
            values[idx] = 0;
        }
    }

    /* ============================================================
       PASS 2: Scatter (run, value) pairs to the output buffer.
       Uses bulk memset for zero runs and direct writes for values.

       Strategy: clear the output once upfront, then just write nonzero
       values. Run lengths just advance the position pointer — the zeros
       are already there from the memset.
       ============================================================ */

    if (pitch_elems == width) {
        /* Fast path: output is contiguous 1D — no pitch gaps.
           Single memset, then scatter nonzero values only. */
        size_t total_pixels = (size_t)width * (size_t)height;
        memset(data, 0, total_pixels * sizeof(int32_t));

        int pos = 0;
        for (int i = 0; i < pair_count; i++) {
            pos += runs[i];
            if (values[i] != 0 && pos < (int)total_pixels) {
                data[pos] = values[i];
                pos++;
            }
        }
    } else {
        /* General path: 2D output with pitch gaps.
           Clear each row individually, then scatter values. */
        for (int row = 0; row < height; row++)
            memset(data + row * pitch_elems, 0, width * sizeof(int32_t));

        int row = 0, col = 0;
        for (int i = 0; i < pair_count && row < height; i++) {
            /* Advance by run length */
            col += runs[i];
            while (col >= width && row < height) { col -= width; row++; }

            if (values[i] != 0 && row < height) {
                if (col < width)
                    data[row * pitch_elems + col] = values[i];
                col++;
                if (col >= width) { row++; col = 0; }
            }
        }
    }

    /* TLS buffer is reused across calls unless disabled for sanitizer runs. */
    JANS_DECODE_RETURN(0);
    #undef JANS_DECODE_RETURN
}

/* ================================================================
   Original single-pass x4 decode (kept for A/B benchmarking)
   ================================================================ */

int jans_decode_band_x4_onepass(const uint8_t *in_buf, size_t in_size,
                        int32_t *data, int width, int height, int pitch) {
    if (in_size < 16) return -1;
    int pitch_elems = pitch / sizeof(int32_t);

    const uint8_t *p = in_buf;
    int token_count = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int freq_size   = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int rans_size   = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int resid_size  = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;

    if (token_count < 0 || freq_size < 0 || rans_size < (int)(4*JANS_INTERLEAVE) || resid_size < 0) return -1;
    if ((size_t)16 + (size_t)freq_size + (size_t)rans_size + (size_t)resid_size > in_size) return -1;
    if (freq_size < JANS_NUM_SYMBOLS * 2) return -1;

    const uint8_t *freq_data = p; p += freq_size;
    JANS_TABLE table;
    memset(&table, 0, sizeof(table));
    for (int i = 0; i < JANS_NUM_SYMBOLS && i*2+1 < freq_size; i++)
        table.freq[i] = ((uint16_t)freq_data[i*2] << 8) | freq_data[i*2+1];
    normalize_freq(table.freq, JANS_NUM_SYMBOLS);
    build_tables(&table, JANS_NUM_SYMBOLS);

    const uint8_t *rans_data = p; p += rans_size;
    const uint8_t *rans_end = rans_data + rans_size;
    const uint8_t *resid_data = p;

    /* Initialize 4 states */
    uint32_t states[JANS_INTERLEAVE];
    const uint8_t *rptr = rans_data;
    for (int s = 0; s < JANS_INTERLEAVE; s++) {
        states[s] = ((uint32_t)rptr[0]<<24) | ((uint32_t)rptr[1]<<16) |
                    ((uint32_t)rptr[2]<<8)  | (uint32_t)rptr[3];
        rptr += 4;
    }

    for (int row = 0; row < height; row++)
        memset(data + row * pitch_elems, 0, width * sizeof(int32_t));

    size_t resid_byte = 0;
    int resid_bit = 0;
    int row = 0, col = 0;

    /* Ultra-fast decode loop using precomputed decode_info table.
       Per token: 1 table lookup (decode_info), 1 rANS state update,
       1 renorm, 1 merged bitbuf_read, 1 output store.
       Eliminates: sym/16, sym%16, class_to_run, class_to_mag, 2 extra bitbuf_reads. */
    int t = 0;

    /* Precompute row start pointers for scatter-free output */
    int32_t *row_ptr = data;

    while (t + JANS_INTERLEAVE <= token_count && row < height)
    {
        /* Fully unrolled 4-state decode: interleave lookups with renorms
           to hide memory latency and multiply latency. */
        const JANS_DECODE_INFO *infos[4];
        uint32_t slots[4];

        /* Phase A: All 4 table lookups (back-to-back, pipelined) */
        slots[0] = states[0] & (JANS_TABLE_SIZE - 1);
        slots[1] = states[1] & (JANS_TABLE_SIZE - 1);
        slots[2] = states[2] & (JANS_TABLE_SIZE - 1);
        slots[3] = states[3] & (JANS_TABLE_SIZE - 1);
        infos[0] = &table.decode_info[slots[0]];
        infos[1] = &table.decode_info[slots[1]];
        infos[2] = &table.decode_info[slots[2]];
        infos[3] = &table.decode_info[slots[3]];

        /* Phase B: All 4 state updates (multiply can pipeline across states) */
        states[0] = infos[0]->freq * (states[0] >> JANS_TABLE_BITS) + slots[0] - infos[0]->cum_freq;
        states[1] = infos[1]->freq * (states[1] >> JANS_TABLE_BITS) + slots[1] - infos[1]->cum_freq;
        states[2] = infos[2]->freq * (states[2] >> JANS_TABLE_BITS) + slots[2] - infos[2]->cum_freq;
        states[3] = infos[3]->freq * (states[3] >> JANS_TABLE_BITS) + slots[3] - infos[3]->cum_freq;

        /* Phase C: All 4 renorms (serial per state, but each is short) */
        for (int s = 0; s < 4; s++) {
            while (states[s] < RANS_BYTE_L) {
                if (rptr >= rans_end) return -1;
                states[s] = (states[s] << 8) | *rptr++;
            }
        }

        /* Prefetch next iteration's table entries */
        __builtin_prefetch(&table.decode_info[states[0] & (JANS_TABLE_SIZE - 1)], 0, 3);
        __builtin_prefetch(&table.decode_info[states[1] & (JANS_TABLE_SIZE - 1)], 0, 3);
        __builtin_prefetch(&table.decode_info[states[2] & (JANS_TABLE_SIZE - 1)], 0, 3);
        __builtin_prefetch(&table.decode_info[states[3] & (JANS_TABLE_SIZE - 1)], 0, 3);

        /* Process 4 decoded tokens */
        for (int s = 0; s < JANS_INTERLEAVE && row < height; s++) {
            const JANS_DECODE_INFO *di = infos[s];

            /* Single merged bitbuf_read for ALL residual bits (run + mag + sign) */
            uint32_t all_bits = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit,
                                            di->total_bits);

            /* Extract run from bottom run_bits */
            int run = di->run_min + (all_bits & ((1u << di->run_bits) - 1));
            all_bits >>= di->run_bits;

            /* Advance position by run */
            col += run;
            while (col >= width) { col -= width; row++; row_ptr = data + row * pitch_elems; }

            if (di->has_value && row < height) {
                /* Extract magnitude from next mag_bits */
                int mag = di->mag_min + (all_bits & ((1u << di->mag_bits) - 1));
                all_bits >>= di->mag_bits;

                /* Extract sign from next 1 bit */
                int sign = all_bits & 1;

                if (col < width)
                    row_ptr[col] = sign ? -mag : mag;
                col++;
                if (col >= width) { row++; col = 0; row_ptr = data + row * pitch_elems; }
            }
        }
        t += JANS_INTERLEAVE;
    }

    /* Handle remaining tokens (< 4) */
    for (; t < token_count && row < height; t++) {
        int s = t & (JANS_INTERLEAVE - 1);
        uint32_t slot = states[s] & (JANS_TABLE_SIZE - 1);
        const JANS_DECODE_INFO *di = &table.decode_info[slot];
        states[s] = di->freq * (states[s] >> JANS_TABLE_BITS) + slot - di->cum_freq;
        while (states[s] < RANS_BYTE_L) {
            if (rptr >= rans_end) return -1;
            states[s] = (states[s] << 8) | *rptr++;
        }
        uint32_t all_bits = bitbuf_read(resid_data, resid_size, &resid_byte, &resid_bit,
                                        di->total_bits);
        int run = di->run_min + (all_bits & ((1u << di->run_bits) - 1));
        all_bits >>= di->run_bits;
        col += run;
        while (col >= width) { col -= width; row++; row_ptr = data + row * pitch_elems; }
        if (di->has_value && row < height) {
            int mag = di->mag_min + (all_bits & ((1u << di->mag_bits) - 1));
            all_bits >>= di->mag_bits;
            int sign = all_bits & 1;
            if (col < width)
                row_ptr[col] = sign ? -mag : mag;
            col++;
            if (col >= width) { row++; col = 0; row_ptr = data + row * pitch_elems; }
        }
    }

    return 0;
}
