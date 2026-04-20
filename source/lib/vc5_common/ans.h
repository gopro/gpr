/*! @file ans.h
 *
 *  @brief Asymmetric Numeral Systems (ANS) entropy coder for wavelet coefficients.
 *
 *  Adaptive entropy coder that replaces the fixed VLC (Table17) codebook.
 *  Uses tANS (table-based ANS) with per-band frequency tables built from
 *  actual coefficient statistics, achieving near-optimal compression.
 *
 *  The coding scheme handles the same RLE+magnitude pattern as the VLC:
 *  - Zero runs are encoded with a geometric distribution model
 *  - Non-zero magnitudes use a Laplacian/exponential model
 *  - Sign bits are raw (1 bit each, 50/50 probability)
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *  Licensed under Apache-2.0 or MIT at your option.
 */

#ifndef ANS_H
#define ANS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! ANS table size (must be power of 2). Larger = better compression, more memory.
    2048 gives excellent compression with ~8KB tables. */
#define ANS_TABLE_BITS  12
#define ANS_TABLE_SIZE  (1 << ANS_TABLE_BITS)

/*! Maximum symbol value for magnitude coding.
    Must be >= 1023 to handle uncompanded wavelet coefficients in raw mode.
    Magnitudes above this are clipped. */
#define ANS_MAX_SYMBOL  1024

/*! Number of symbols: 0..ANS_MAX_SYMBOL plus escape */
#define ANS_NUM_SYMBOLS (ANS_MAX_SYMBOL + 2)

/*! ANS encoder state */
typedef struct {
    uint32_t state;         /* Current ANS state */
    uint8_t *out_buf;       /* Output buffer (written backwards) */
    uint8_t *out_ptr;       /* Current write position */
    size_t   out_capacity;  /* Buffer capacity */
} ANS_ENCODER;

/*! ANS decoder state */
typedef struct {
    uint32_t state;         /* Current ANS state */
    const uint8_t *in_buf;  /* Input buffer */
    const uint8_t *in_ptr;  /* Current read position */
    const uint8_t *in_end;  /* One past last valid byte */
} ANS_DECODER;

/*! Frequency table for one symbol distribution */
typedef struct {
    uint16_t freq[ANS_NUM_SYMBOLS];     /* Symbol frequencies (sum = ANS_TABLE_SIZE) */
    uint16_t cum_freq[ANS_NUM_SYMBOLS]; /* Cumulative frequencies */

    /* tANS encoding table: for each (state, symbol) → new state + output bits */
    /* Decoding table: for each state → (symbol, new state) */
    uint16_t decode_sym[ANS_TABLE_SIZE];    /* symbol for each table entry */
    uint16_t decode_next[ANS_TABLE_SIZE];   /* next state for each table entry */
} ANS_TABLE;

/*! Per-band ANS context: separate tables for runs and magnitudes */
typedef struct {
    ANS_TABLE run_table;    /* Zero run-length distribution */
    ANS_TABLE mag_table;    /* Non-zero magnitude distribution */
    int       initialized;
} ANS_BAND_CTX;

/*!
    @brief Build ANS frequency tables from wavelet coefficient statistics

    Scans the band data and builds optimal frequency tables for both
    zero runs and non-zero magnitudes.

    @param ctx      Band context to initialize
    @param data     Quantized wavelet band data
    @param width    Band width
    @param height   Band height
    @param pitch    Row stride in bytes
*/
void ans_build_tables(ANS_BAND_CTX *ctx, const int32_t *data,
                      int width, int height, int pitch);

/*!
    @brief Encode a highpass band using ANS

    Drop-in replacement for EncodeHighpassBandRowRuns.
    Encodes the band data using adaptive ANS tables, then writes
    the compressed data to the bitstream.

    @param out_buf      Output buffer for compressed data
    @param out_capacity Buffer capacity
    @param ctx          Pre-built ANS tables for this band
    @param data         Quantized wavelet band data
    @param width        Band width
    @param height       Band height
    @param pitch        Row stride in bytes
    @return             Number of bytes written, or -1 on error
*/
int ans_encode_band(uint8_t *out_buf, size_t out_capacity,
                    const ANS_BAND_CTX *ctx, const int32_t *data,
                    int width, int height, int pitch);

/*!
    @brief Decode a highpass band using ANS

    Drop-in replacement for the VLC band decoder.

    @param in_buf       Compressed data buffer
    @param in_size      Compressed data size
    @param ctx          ANS tables (must match encoder)
    @param data         Output buffer for decoded band
    @param width        Band width
    @param height       Band height
    @param pitch        Row stride in bytes
    @return             0 on success, -1 on error
*/
int ans_decode_band(const uint8_t *in_buf, size_t in_size,
                    const ANS_BAND_CTX *ctx, int32_t *data,
                    int width, int height, int pitch);

/*!
    @brief Serialize ANS frequency tables to a buffer

    The tables must be stored in the bitstream so the decoder can rebuild them.

    @param ctx          Band context with initialized tables
    @param out_buf      Output buffer
    @param out_capacity Buffer capacity
    @return             Bytes written, or -1 on error
*/
int ans_serialize_tables(const ANS_BAND_CTX *ctx, uint8_t *out_buf, size_t out_capacity);

/*!
    @brief Deserialize ANS frequency tables from a buffer

    @param ctx          Band context to initialize
    @param in_buf       Input buffer
    @param in_size      Buffer size
    @return             Bytes consumed, or -1 on error
*/
int ans_deserialize_tables(ANS_BAND_CTX *ctx, const uint8_t *in_buf, size_t in_size);

#ifdef __cplusplus
}
#endif

#endif /* ANS_H */
