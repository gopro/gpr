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

typedef struct {
    uint16_t freq[JANS_NUM_SYMBOLS + 1];
    uint16_t cum_freq[JANS_NUM_SYMBOLS + 1];
    uint16_t decode_sym[JANS_TABLE_SIZE];
    JANS_DECODE_ENTRY decode_fast[JANS_TABLE_SIZE]; /* Packed for fast decode */
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

/*!
    @brief Decode 4-way interleaved rANS band.
    @return 0 on success, -1 on error.
*/
int jans_decode_band_x4(const uint8_t *in_buf, size_t in_size,
                        int32_t *data, int width, int height, int pitch);

#ifdef __cplusplus
}
#endif

#endif /* ANS_JOINT_H */
