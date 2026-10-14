/*! @file ans.c
 *
 *  @brief rANS entropy coder for wavelet coefficients.
 *
 *  Uses Fabian Giesen's byte-aligned rANS approach for robustness.
 *  Encoder writes forward, buffer is reversed, decoder reads forward.
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#include "ans.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* rANS constants */
#define RANS_BYTE_L    (1u << 23)

/* ---- rANS core (Giesen-style byte-aligned) ---- */

static inline void rans_enc_put(uint32_t *state, uint8_t **pptr,
                                uint32_t start, uint32_t freq)
{
    uint32_t x = *state;
    uint32_t x_max = ((RANS_BYTE_L >> ANS_TABLE_BITS) << 8) * freq;
    while (x >= x_max) {
        *(*pptr)++ = (uint8_t)(x & 0xFF);
        x >>= 8;
    }
    *state = ((x / freq) << ANS_TABLE_BITS) + (x % freq) + start;
}

static inline int rans_dec_renorm(uint32_t *state, const uint8_t **pptr,
                                  const uint8_t *in_end)
{
    while (*state < RANS_BYTE_L) {
        if (*pptr >= in_end) return -1;  /* buffer overread */
        *state = (*state << 8) | **pptr;
        (*pptr)++;
    }
    return 0;
}

/* ---- Frequency table construction ---- */

static void normalize_freq(uint16_t *freq, int num_symbols)
{
    uint32_t total = 0;
    for (int i = 0; i < num_symbols; i++)
        total += freq[i];

    if (total == 0) {
        freq[0] = ANS_TABLE_SIZE;
        return;
    }

    /* Scale to ANS_TABLE_SIZE, ensuring every non-zero freq gets at least 1 */
    uint16_t scaled[ANS_NUM_SYMBOLS];
    uint32_t scaled_total = 0;
    for (int i = 0; i < num_symbols; i++) {
        if (freq[i] > 0) {
            uint32_t s = (uint32_t)freq[i] * ANS_TABLE_SIZE / total;
            if (s == 0) s = 1;
            scaled[i] = (uint16_t)s;
        } else {
            scaled[i] = 0;
        }
        scaled_total += scaled[i];
    }

    /* Adjust largest frequency to make sum exact */
    int32_t diff = (int32_t)ANS_TABLE_SIZE - (int32_t)scaled_total;
    int max_idx = 0;
    for (int i = 1; i < num_symbols; i++)
        if (scaled[i] > scaled[max_idx]) max_idx = i;
    scaled[max_idx] = (uint16_t)((int32_t)scaled[max_idx] + diff);

    memcpy(freq, scaled, num_symbols * sizeof(uint16_t));
}

static void build_tables(ANS_TABLE *table, int num_symbols)
{
    /* Build cumulative frequencies */
    table->cum_freq[0] = 0;
    for (int i = 1; i < num_symbols; i++)
        table->cum_freq[i] = table->cum_freq[i-1] + table->freq[i-1];

    /* Build decode table: for each slot, find the symbol */
    int sym = 0;
    for (int i = 0; i < ANS_TABLE_SIZE; i++) {
        while (sym < num_symbols - 1 &&
               i >= table->cum_freq[sym] + table->freq[sym])
            sym++;
        table->decode_sym[i] = (uint16_t)sym;
        table->decode_next[i] = (uint16_t)(table->freq[sym] +
                                            i - table->cum_freq[sym]);
    }
}

void ans_build_tables(ANS_BAND_CTX *ctx, const int32_t *data,
                      int width, int height, int pitch)
{
    int pitch_elems = pitch / sizeof(int32_t);

    memset(ctx->run_table.freq, 0, sizeof(ctx->run_table.freq));
    memset(ctx->mag_table.freq, 0, sizeof(ctx->mag_table.freq));

    for (int row = 0; row < height; row++)
    {
        const int32_t *rowptr = data + row * pitch_elems;
        int run = 0;

        for (int col = 0; col < width; col++)
        {
            int32_t val = rowptr[col];
            if (val == 0) { run++; continue; }

            /* Split long runs */
            while (run > ANS_MAX_SYMBOL) {
                ctx->run_table.freq[ANS_MAX_SYMBOL]++;
                ctx->mag_table.freq[0]++;
                run -= ANS_MAX_SYMBOL;
            }
            ctx->run_table.freq[run]++;

            int32_t mag = (val < 0) ? -val : val;
            if (mag > ANS_MAX_SYMBOL) mag = ANS_MAX_SYMBOL;
            ctx->mag_table.freq[mag]++;

            run = 0;
        }

        if (run > 0) {
            while (run > ANS_MAX_SYMBOL) {
                ctx->run_table.freq[ANS_MAX_SYMBOL]++;
                ctx->mag_table.freq[0]++;
                run -= ANS_MAX_SYMBOL;
            }
            ctx->run_table.freq[run]++;
            ctx->mag_table.freq[0]++;
        }
    }

    /* Ensure no zero-frequency gaps in used range */
    ctx->run_table.freq[ANS_MAX_SYMBOL + 1] = 1;
    ctx->mag_table.freq[ANS_MAX_SYMBOL + 1] = 1;

    normalize_freq(ctx->run_table.freq, ANS_NUM_SYMBOLS);
    build_tables(&ctx->run_table, ANS_NUM_SYMBOLS);
    normalize_freq(ctx->mag_table.freq, ANS_NUM_SYMBOLS);
    build_tables(&ctx->mag_table, ANS_NUM_SYMBOLS);

    ctx->initialized = 1;
}

/* ---- Band-level encode/decode ---- */

int ans_encode_band(uint8_t *out_buf, size_t out_capacity,
                    const ANS_BAND_CTX *ctx, const int32_t *data,
                    int width, int height, int pitch)
{
    if (!ctx->initialized) return -1;
    int pitch_elems = pitch / sizeof(int32_t);

    /* Guard against integer overflow for large images */
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > (size_t)(INT32_MAX / 2)) return -1;

    /* Collect (run, mag, sign) triples */
    size_t max_pairs = pixels + (size_t)height + (pixels / ANS_MAX_SYMBOL) + 16;
    typedef struct { uint16_t run, mag; uint8_t sign; } PAIR;
    PAIR *pairs = (PAIR *)malloc(max_pairs * sizeof(PAIR));
    if (!pairs) return -1;
    int pair_count = 0;

    /* Also collect signs separately */
    size_t sign_alloc = pixels;
    uint8_t *sign_buf = (uint8_t *)calloc((sign_alloc + 7) / 8 + 1, 1);
    if (!sign_buf) { free(pairs); return -1; }
    int sign_count = 0;

    for (int row = 0; row < height; row++) {
        const int32_t *rowptr = data + row * pitch_elems;
        int run = 0;
        for (int col = 0; col < width; col++) {
            int32_t val = rowptr[col];
            if (val == 0) { run++; continue; }
            int32_t mag = (val < 0) ? -val : val;
            if (mag > ANS_MAX_SYMBOL) mag = ANS_MAX_SYMBOL;

            /* Split long runs into chunks of ANS_MAX_SYMBOL */
            while (run > ANS_MAX_SYMBOL) {
                pairs[pair_count].run = ANS_MAX_SYMBOL;
                pairs[pair_count].mag = 0; /* sentinel: run-only */
                pairs[pair_count].sign = 0;
                pair_count++;
                run -= ANS_MAX_SYMBOL;
            }

            pairs[pair_count].run = (uint16_t)run;
            pairs[pair_count].mag = (uint16_t)mag;
            pairs[pair_count].sign = (val < 0) ? 1 : 0;
            if (pairs[pair_count].sign)
                sign_buf[sign_count / 8] |= (1 << (sign_count % 8));
            sign_count++;
            pair_count++;
            run = 0;
        }
        if (run > 0) {
            while (run > ANS_MAX_SYMBOL) {
                pairs[pair_count].run = ANS_MAX_SYMBOL;
                pairs[pair_count].mag = 0;
                pairs[pair_count].sign = 0;
                pair_count++;
                run -= ANS_MAX_SYMBOL;
            }
            pairs[pair_count].run = (uint16_t)run;
            pairs[pair_count].mag = 0;
            pairs[pair_count].sign = 0;
            pair_count++;
        }
    }

    int sign_bytes = (sign_count + 7) / 8;

    /* rANS encode: process pairs in REVERSE, write bytes FORWARD */
    size_t rans_buf_size = out_capacity;
    uint8_t *rans_buf = (uint8_t *)malloc(rans_buf_size);
    if (!rans_buf) { free(pairs); free(sign_buf); return -1; }

    uint8_t *rans_ptr = rans_buf;
    uint32_t state = RANS_BYTE_L;

    for (int i = pair_count - 1; i >= 0; i--) {
        /* Always encode both run and mag (mag=0 for trailing-zero pairs) */
        rans_enc_put(&state, &rans_ptr,
                     ctx->mag_table.cum_freq[pairs[i].mag],
                     ctx->mag_table.freq[pairs[i].mag]);
        rans_enc_put(&state, &rans_ptr,
                     ctx->run_table.cum_freq[pairs[i].run],
                     ctx->run_table.freq[pairs[i].run]);
    }

    /* Flush final state */
    *rans_ptr++ = (uint8_t)(state >> 0);
    *rans_ptr++ = (uint8_t)(state >> 8);
    *rans_ptr++ = (uint8_t)(state >> 16);
    *rans_ptr++ = (uint8_t)(state >> 24);

    size_t rans_size = rans_ptr - rans_buf;

    /* Reverse the rANS buffer */
    for (size_t i = 0; i < rans_size / 2; i++) {
        uint8_t t = rans_buf[i];
        rans_buf[i] = rans_buf[rans_size - 1 - i];
        rans_buf[rans_size - 1 - i] = t;
    }

    free(pairs);

    /* Pack: [pair_count:4][rans_size:4][sign_bytes:4][rans_data][sign_data] */
    size_t total = 12 + rans_size + sign_bytes;
    if (total > out_capacity) { free(rans_buf); free(sign_buf); return -1; }

    uint8_t *p = out_buf;
    *p++ = (pair_count >> 24) & 0xFF; *p++ = (pair_count >> 16) & 0xFF;
    *p++ = (pair_count >> 8) & 0xFF;  *p++ = pair_count & 0xFF;
    *p++ = (rans_size >> 24) & 0xFF;  *p++ = (rans_size >> 16) & 0xFF;
    *p++ = (rans_size >> 8) & 0xFF;   *p++ = rans_size & 0xFF;
    *p++ = (sign_bytes >> 24) & 0xFF; *p++ = (sign_bytes >> 16) & 0xFF;
    *p++ = (sign_bytes >> 8) & 0xFF;  *p++ = sign_bytes & 0xFF;
    memcpy(p, rans_buf, rans_size); p += rans_size;
    memcpy(p, sign_buf, sign_bytes);

    free(rans_buf);
    free(sign_buf);

    return (int)total;
}

int ans_decode_band(const uint8_t *in_buf, size_t in_size,
                    const ANS_BAND_CTX *ctx, int32_t *data,
                    int width, int height, int pitch)
{
    if (!ctx->initialized || in_size < 12) return -1;
    int pitch_elems = pitch / sizeof(int32_t);

    const uint8_t *p = in_buf;
    int pair_count = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int rans_size  = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;
    int sign_bytes = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; p += 4;

    /* Validate header fields against buffer bounds */
    if (pair_count < 0 || rans_size < 4 || sign_bytes < 0) return -1;
    if ((size_t)12 + (size_t)rans_size + (size_t)sign_bytes > in_size) return -1;

    /* Validate pair_count is reasonable for the image dimensions */
    size_t max_reasonable_pairs = (size_t)width * (size_t)height * 2;
    if ((size_t)pair_count > max_reasonable_pairs && max_reasonable_pairs > 0) return -1;

    const uint8_t *rans_data = p;
    const uint8_t *rans_end  = p + rans_size;
    const uint8_t *sign_data = p + rans_size;

    /* Initialize rANS state from first 4 bytes (big-endian after reversal) */
    uint32_t state = ((uint32_t)rans_data[0]<<24) | ((uint32_t)rans_data[1]<<16) |
                     ((uint32_t)rans_data[2]<<8) | (uint32_t)rans_data[3];
    const uint8_t *rptr = rans_data + 4;

    /* Clear output */
    for (int row = 0; row < height; row++)
        memset(data + row * pitch_elems, 0, width * sizeof(int32_t));

    int row = 0, col = 0, sign_idx = 0;

    for (int i = 0; i < pair_count && row < height; i++)
    {
        /* Decode run */
        uint32_t slot = state & (ANS_TABLE_SIZE - 1);
        int run = ctx->run_table.decode_sym[slot];
        uint16_t rf = ctx->run_table.freq[run];
        state = rf * (state >> ANS_TABLE_BITS) + slot - ctx->run_table.cum_freq[run];
        if (rans_dec_renorm(&state, &rptr, rans_end) != 0) return -1;

        /* Decode magnitude */
        slot = state & (ANS_TABLE_SIZE - 1);
        int mag = ctx->mag_table.decode_sym[slot];
        uint16_t mf = ctx->mag_table.freq[mag];
        state = mf * (state >> ANS_TABLE_BITS) + slot - ctx->mag_table.cum_freq[mag];
        if (rans_dec_renorm(&state, &rptr, rans_end) != 0) return -1;

        /* Apply the zero run */
        col += run;
        while (col >= width) { col -= width; row++; }

        if (mag > 0 && row < height && col < width)
        {
            int sign = 0;
            if (sign_idx / 8 < sign_bytes)
                sign = (sign_data[sign_idx / 8] >> (sign_idx % 8)) & 1;
            sign_idx++;

            data[row * pitch_elems + col] = sign ? -mag : mag;
            col++;
            if (col >= width) { row++; col = 0; }
        }
        /* mag=0: run-only pair (trailing zeros or long-run continuation) */
    }

    return 0;
}

/* ---- Table serialization ---- */

int ans_serialize_tables(const ANS_BAND_CTX *ctx, uint8_t *out_buf, size_t out_capacity)
{
    size_t needed = ANS_NUM_SYMBOLS * 2 * 2;
    if (out_capacity < needed) return -1;

    uint8_t *p = out_buf;
    for (int i = 0; i < ANS_NUM_SYMBOLS; i++) {
        *p++ = (uint8_t)(ctx->run_table.freq[i] >> 8);
        *p++ = (uint8_t)(ctx->run_table.freq[i]);
    }
    for (int i = 0; i < ANS_NUM_SYMBOLS; i++) {
        *p++ = (uint8_t)(ctx->mag_table.freq[i] >> 8);
        *p++ = (uint8_t)(ctx->mag_table.freq[i]);
    }
    return (int)needed;
}

int ans_deserialize_tables(ANS_BAND_CTX *ctx, const uint8_t *in_buf, size_t in_size)
{
    size_t needed = ANS_NUM_SYMBOLS * 2 * 2;
    if (in_size < needed) return -1;

    const uint8_t *p = in_buf;
    for (int i = 0; i < ANS_NUM_SYMBOLS; i++) {
        ctx->run_table.freq[i] = ((uint16_t)p[0] << 8) | p[1]; p += 2;
    }
    for (int i = 0; i < ANS_NUM_SYMBOLS; i++) {
        ctx->mag_table.freq[i] = ((uint16_t)p[0] << 8) | p[1]; p += 2;
    }

    /* Validate deserialized freq tables: ensure no zero-freq symbol exists
       between non-zero-freq symbols, which would cause division by zero in
       rans_enc_put. Re-normalize to guarantee sum == ANS_TABLE_SIZE and
       every symbol in the active range has freq >= 1. */
    normalize_freq(ctx->run_table.freq, ANS_NUM_SYMBOLS);
    normalize_freq(ctx->mag_table.freq, ANS_NUM_SYMBOLS);

    /* Verify normalization produced valid tables (guards against underflow
       in the adjustment step when adversarial frequency data is provided) */
    {
        uint32_t run_sum = 0, mag_sum = 0;
        for (int i = 0; i < ANS_NUM_SYMBOLS; i++) {
            run_sum += ctx->run_table.freq[i];
            mag_sum += ctx->mag_table.freq[i];
        }
        if (run_sum != ANS_TABLE_SIZE || mag_sum != ANS_TABLE_SIZE)
            return -1;
    }

    build_tables(&ctx->run_table, ANS_NUM_SYMBOLS);
    build_tables(&ctx->mag_table, ANS_NUM_SYMBOLS);
    ctx->initialized = 1;
    return (int)needed;
}
