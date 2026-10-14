/*! @file ans_joint.h
 *  @brief Joint RLV (run-length-value) ANS coder.
 *
 *  Encodes (run, magnitude, sign) triples as SINGLE ANS symbols,
 *  matching VLC's efficiency. Uses token + residual bits design:
 *  - Token: ANS-coded symbol representing (run_class, mag_class)
 *  - Residual: raw bits for exact run and magnitude within class
 *  - Sign: 1 raw bit per nonzero coefficient
 *
 *  This eliminates the 2-symbol-per-coefficient overhead of the
 *  separate run+magnitude ANS approach.
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

#ifndef ANS_JOINT_H
#define ANS_JOINT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Run classes: 0-3 direct, 4-9 use exponentially increasing extra bits */
#define JANS_RUN_CLASSES 10
/* Mag classes: 0-7 direct, 8-15 use 1-8 extra bits */
#define JANS_MAG_CLASSES 16
/* Total joint symbols: run_class × mag_class */
#define JANS_NUM_SYMBOLS (JANS_RUN_CLASSES * JANS_MAG_CLASSES)  /* 128 */
/* +1 for end-of-band marker */
#define JANS_TABLE_BITS  11
#define JANS_TABLE_SIZE  (1 << JANS_TABLE_BITS)  /* 2048 — plenty for 128 symbols */

/*! Packed decode entry: one cache-line-friendly lookup per slot */
typedef struct {
    uint16_t sym;       /* Decoded symbol */
    uint16_t freq;      /* Symbol frequency */
    uint16_t cum_freq;  /* Cumulative frequency */
    uint16_t _pad;
} JANS_DECODE_ENTRY;

/*! Ultra-packed decode entry: ALL per-token info in one 16-byte struct.
    Eliminates per-token division, class lookup, and class_to_run/mag. */
typedef struct {
    uint16_t freq;        /* Symbol frequency for rANS state update */
    uint16_t cum_freq;    /* Cumulative frequency for rANS state update */
    uint8_t  total_bits;  /* run_bits + mag_bits + (mc>0 ? 1 : 0) = total resid bits */
    uint8_t  run_bits;    /* Extra bits for run class */
    uint8_t  mag_bits;    /* Extra bits for mag class */
    uint8_t  has_value;   /* 1 if mc > 0 (nonzero coefficient) */
    uint16_t run_min;     /* Minimum run for this class */
    uint16_t mag_min;     /* Minimum magnitude for this class */
} JANS_DECODE_INFO;

/*! Packed per-symbol encode entry. 8 bytes — one cacheline-fitted load
    pulls all 3 values rans_enc_put needs per symbol (was 3 loads from 3
    separate arrays scattered across the table). */
typedef struct {
    uint16_t freq;
    uint16_t cum_freq;
    uint32_t rcp_freq;
} JANS_ENC_ENTRY;

typedef struct {
    uint16_t freq[JANS_NUM_SYMBOLS + 1];
    uint16_t cum_freq[JANS_NUM_SYMBOLS + 1];
    uint32_t rcp_freq[JANS_NUM_SYMBOLS + 1];  /* Reciprocal: ceil(2^32 / freq) for division-free encode */
    JANS_ENC_ENTRY enc[JANS_NUM_SYMBOLS + 1]; /* Packed encode-side {freq,cum_freq,rcp_freq} for tight rANS inner loop */
    uint16_t decode_sym[JANS_TABLE_SIZE];
    JANS_DECODE_ENTRY decode_fast[JANS_TABLE_SIZE]; /* Packed for fast decode */
    JANS_DECODE_INFO decode_info[JANS_TABLE_SIZE]; /* Ultra-packed: all per-token info */
    int initialized;
} JANS_TABLE;

/*!
    @brief Encode a highpass band using joint RLV ANS.
    Single symbol per coefficient — matches VLC's efficiency.

    @return Number of bytes written, or -1 on error.
*/
int jans_encode_band(uint8_t *out_buf, size_t out_capacity,
                     const int32_t *data, int width, int height, int pitch);

/*!
    @brief Decode a highpass band using joint RLV ANS.
    @return 0 on success, -1 on error.
*/
int jans_decode_band(const uint8_t *in_buf, size_t in_size,
                     int32_t *data, int width, int height, int pitch);

/*!
    @brief Encode using 4-way interleaved rANS for parallel decode.
    Same blob format header but rANS data uses 4 interleaved states.
    Bitwise identical compression to jans_encode_band.

    @return Number of bytes written, or -1 on error.
*/
int jans_encode_band_x4(uint8_t *out_buf, size_t out_capacity,
                        const int32_t *data, int width, int height, int pitch);

/* ================================================================
   Inline-tokenize API — for the fused encoder's "memory-tight" mode.

   Lets a producer (Pass 1's vert+quant) tokenize each quantized band row
   immediately, without first materializing the whole band as a buffer.
   The Pass 2 phase then only does rANS encode + output, no tokenize.

   Saves the full band_data buffer (~5.8 MB per band × 12 bands = ~70 MB
   at 23 MP, ~150 MB at 50 MP). The trade-off is that tokenize work moves
   from 12 Pass 2 threads into 4 Pass 1 threads — neutral or a small loss
   on wide-core machines (M1), neutral on 4-core embedded.

   Lifecycle:
       state = jans_inline_create(max_coeffs);
       jans_inline_reset(state);             // per-frame
       for each band row k:
           jans_inline_row(state, row, width);
       int n = jans_inline_finalize(out_buf, out_cap, state);
       jans_inline_destroy(state);           // at program end
   ================================================================ */
typedef struct JANS_INLINE_STATE JANS_INLINE_STATE;

JANS_INLINE_STATE *jans_inline_create(size_t max_coeffs);

/*! Enable stripe mode: emit num_stripes independent rANS sub-blobs per band.
    Tokens/resid/rANS buffers only need to hold one stripe's worth at a time —
    cuts peak memory in the encoder by ~(num_stripes - 1)/num_stripes.

    Format produced by jans_inline_finalize when stripe_rows > 0:
        marker (4 bytes) = 0xFFFFFFFF (illegal as a token_count, signals stripe mode)
        num_stripes (4 bytes)
        for each stripe:
            stripe_row_count (4 bytes)   -- band rows covered by this stripe
            stripe_size      (4 bytes)   -- byte length of stripe_data
            stripe_data      (stripe_size bytes)  -- single-blob format

    jans_decode_band_x4 detects the 0xFFFFFFFF marker and decodes accordingly.

    Pass stripe_rows = 0 to disable (default: single-blob output, backward
    compatible with the old format). */
void jans_inline_set_stripe_rows(JANS_INLINE_STATE *s, int stripe_rows);

/*! Enable deferred-rANS mode (only meaningful when stripe_rows > 0).
    When enabled, jans_inline_row stops running the rANS encode at stripe
    boundaries; instead it snapshots the per-stripe (tokens, freqs,
    resid-bits) and lets jans_inline_finalize do the rANS work in one shot.
    This shifts ~6-12 ms of rANS work out of the Pass 1 critical path and
    into Pass 2 (where the band-parallel thread already handles finalize).

    The encoded output bytes are bit-identical to the non-deferred path. */
void jans_inline_set_defer_rans(JANS_INLINE_STATE *s, int defer);

/*! Optional diagnostic label emitted by JANS_INLINE_PROFILE_RUNTIME.
    No effect on encoded bytes or non-profile builds. */
void jans_inline_set_profile_label(JANS_INLINE_STATE *s, const char *label);

void jans_inline_reset(JANS_INLINE_STATE *s);
void jans_inline_row(JANS_INLINE_STATE *s, const int32_t *row, int width);
int  jans_inline_finalize(uint8_t *out_buf, size_t out_cap, JANS_INLINE_STATE *s);
void jans_inline_destroy(JANS_INLINE_STATE *s);

/*!
    @brief Decode 4-way interleaved rANS band.
    @return 0 on success, -1 on error.
*/
int jans_decode_band_x4(const uint8_t *in_buf, size_t in_size,
                        int32_t *data, int width, int height, int pitch);

/*!
    @brief Original single-pass decode (kept for A/B benchmarking).
    @return 0 on success, -1 on error.
*/
int jans_decode_band_x4_onepass(const uint8_t *in_buf, size_t in_size,
                                int32_t *data, int width, int height, int pitch);

#ifdef __cplusplus
}
#endif

#endif /* ANS_JOINT_H */
