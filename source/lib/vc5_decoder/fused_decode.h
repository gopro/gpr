/*! @file fused_decode.h
 *
 *  @brief Decoder for the fused encoder's self-describing wrapper format.
 *
 *  Pairs with the encoder in vc5_encoder/fused_encode.{c,h}. Reads the
 *  FUSED_HEADER + band manifest, decodes each rANS band stream via
 *  jans_decode_band_x4, inverse-quantizes and inverse-transforms the
 *  encoded wavelet levels per channel, reverses the 4-channel color
 *  transform, applies the inverse log curve, and packs the result
 *  back into an RGGB / GBRG Bayer plane.
 *
 *  Single-level streams with preserved LL (16 bands), two-level streams
 *  (28 bands), and three-level streams (40 bands) can be reconstructed.
 *  Single-level highpass-only streams do not contain a complete image.
 */
#ifndef FUSED_DECODE_H
#define FUSED_DECODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode a fused-encoder wrapped output back to a Bayer plane.
   enc/enc_size:   the wrapper format produced by gpr_encode_fused().
   bayer_out:      caller-allocated uint16 buffer of size
                   width * height * 2 bytes.
   bayer_pitch:    row stride in bytes (typically width * 2).
   out_width/out_height: decoded dimensions, including encoded decimation.
   Returns 0 on success, negative errno-like on failure. */
int gpr_decode_fused(const uint8_t *enc, size_t enc_size,
                     uint16_t *bayer_out, size_t bayer_pitch_bytes,
                     int *out_width, int *out_height);

/* Half-resolution decode. Skips the level-1 inverse wavelet step, producing
   a bayer plane at (header.width / 2) × (header.height / 2). The level-1
   highpass bands stay in the bitstream — this path doesn't reconstruct them,
   so it's roughly 1.5–2× faster than full-resolution decode and produces
   output sized to feed a downstream CNN at codec-half-res (the pre-FUSED
   GPRCodec topology). bayer_out must be large enough for the half-res output;
   bayer_pitch_bytes is typically (header.width / 2) * 2. Returns 0 on
   success, negative on failure.

   Used by the gpr2prores playback pipeline. Only the multi-level path
   supports half_res; single-level + LL streams ignore the flag and decode
   at native dims. */
int gpr_decode_fused_halfres(const uint8_t *enc, size_t enc_size,
                             uint16_t *bayer_out, size_t bayer_pitch_bytes,
                             int *out_width, int *out_height);

/* Lowpass preview decode for single-level+LL streams. Decodes only the four
   LL bands, skips the single-level inverse wavelet, reverses the channel
   color transform at LL resolution, and returns a Bayer preview at
   (header.width / 2) x (header.height / 2). This is intended for camera
   screen preview from a full 4K Bayer .gvid payload, not for quality render. */
int gpr_decode_fused_ll_preview(const uint8_t *enc, size_t enc_size,
                                uint16_t *bayer_out, size_t bayer_pitch_bytes,
                                int *out_width, int *out_height);

#ifdef __cplusplus
}
#endif

#endif /* FUSED_DECODE_H */
