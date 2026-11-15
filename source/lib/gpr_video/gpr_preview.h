/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_PREVIEW_H
#define GPR_PREVIEW_H
#include "gpr_gvid.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { GPR_PREVIEW_OK=0,GPR_PREVIEW_INVALID=-1,GPR_PREVIEW_CAPACITY=-2,
       GPR_PREVIEW_PAYLOAD=-3,GPR_PREVIEW_CODEC=-4,GPR_PREVIEW_NOMEM=-5,
       GPR_PREVIEW_CLOSED=-6,GPR_PREVIEW_CALLBACK=-7 };
typedef struct {
    uint32_t downsample; /* 4, 8 or 16 Bayer samples per RGB sample axis. */
    uint32_t gain_num[3],gain_pow2_den[3]; /* R,G,B gain = num / 2^den. */
} gpr_preview_options;
typedef struct { uint32_t width,height; size_t row_bytes; } gpr_preview_info;
void gpr_preview_options_default(gpr_preview_options *out);
/* Validate the supported stock RAW VC-5 framing/geometry without decoding. */
int gpr_preview_validate_payload(const gpr_gvid_config *video,const uint8_t *payload,size_t size);
/* Decoder-backed preview requires width/height >=48. */
int gpr_preview_dimensions(const gpr_gvid_config *video,const gpr_preview_options *options,gpr_preview_info *out);
/* Render the complete wavelet-scaled frame to caller-owned RGB24 rows. No CNN,
 * crop or full-resolution RAW intermediate. Display buffers remain caller
 * owned; padding bytes are not modified. Options/default gains are explicit,
 * not camera-calibrated ISP/color science. 4-byte-unaligned payloads are copied
 * to aligned scratch. The fast path accepts the stock SDK's four-channel,
 * three-level RAW profile and checks dimensions/framing before decoding. */
int gpr_preview_render(const gpr_gvid_config *video,const gpr_preview_options *options,
    const uint8_t *payload,size_t payload_size,uint8_t *rgb,size_t capacity,size_t pitch,gpr_preview_info *out);

/* Separate calibrated display contract. The legacy API above remains exact.
 * Coordinates are in the original, unmirrored Bayer image. Matrix is row-major
 * camera RGB -> linear sRGB, including source white balance/adaptation. Black
 * values are Bayer-site counts in row-major CFA order; white uses source counts.
 * Source DNG processing can prepare these small parameters off-camera. The
 * decoder/preview libraries do not acquire a DNG SDK or JSON dependency. */
typedef struct {
    uint32_t phase_mask; /* bits 0..3: original Bayer sites (0,0),(0,1),(1,0),(1,1) */
    uint32_t rows,cols;
    float spacing_y,spacing_x,origin_y,origin_x;
    float area[4]; /* absolute sensor left, top, right, bottom */
    const float *values; /* row-major, one plane; copied/expanded at plan create */
    size_t value_count;
} gpr_preview_gain_map;
typedef struct {
    uint32_t version; /* 1 */
    uint32_t downsample; /* 4, 8, 16 */
    uint32_t orientation; /* EXIF 1..8, after removing the legacy mirror */
    float black[4],white;
    float camera_to_srgb[9];
    float camera_white[3]; /* DNG normalized camera-white clipping before matrix */
    float exposure; /* linear multiplier, not EV */
    float active[4],crop[4]; /* absolute sensor left, top, right, bottom */
    const gpr_preview_gain_map *gain_maps;
    size_t gain_map_count; /* 0..32 */
} gpr_preview_calibration;
typedef struct gpr_preview_plan gpr_preview_plan;
/* Synthetic identity transform, full sensor, no gains/maps, no orientation. */
void gpr_preview_calibration_identity(const gpr_gvid_config *video,gpr_preview_calibration *out);
/* Immutable plan owns its prepared sampling/gain data; caller inputs can expire
 * after creation. May render concurrently. No full-resolution RAW intermediate.
 * Gain maps use bilinear interpolation at reduced sample centers; green sites
 * are averaged after their gains. This is a wavelet preview approximation,
 * not full-resolution demosaicing or a complete DNG/lens/tone renderer. */
int gpr_preview_plan_create(const gpr_gvid_config *video,const gpr_preview_calibration *calibration,gpr_preview_plan **out);
/* Bounded, CRC-checked little-endian GPRP v1 interchange, produced by the
 * optional host profile preparer. No retained references to profile bytes. */
int gpr_preview_plan_from_profile(const gpr_gvid_config *video,const uint8_t *profile,size_t profile_size,gpr_preview_plan **out);
int gpr_preview_plan_info(const gpr_preview_plan *plan,gpr_preview_info *out);
int gpr_preview_plan_render(const gpr_preview_plan *plan,const uint8_t *payload,size_t payload_size,
    uint8_t *rgb,size_t capacity,size_t pitch,gpr_preview_info *out);
/* Render already reconstructed reduced GS/RG/BG planes with the same color,
 * crop and orientation contract as compressed preview. Samples are signed
 * host-endian 16-bit values in the decoder's 14-bit wavelet domain, tightly
 * packed without row padding. Geometry must equal ceil(sensor/downsample).
 * Each plane must cover width*height samples and remain immutable until return.
 * This synchronous entry point does not retain any plane or output pointers.
 * Encoder coefficients require reconstruction to this domain before use. */
typedef struct {
    uint32_t width,height;
    const int16_t *samples[3];
    size_t sample_count[3];
} gpr_preview_wavelet_planes;
int gpr_preview_plan_render_wavelets(const gpr_preview_plan *plan,
    const gpr_preview_wavelet_planes *planes,
    uint8_t *rgb,size_t capacity,size_t pitch,gpr_preview_info *out);
void gpr_preview_plan_destroy(gpr_preview_plan *plan);
/* Acquire another ownership reference. Each reference is released by destroy.
 * The caller must hold a reference while retaining or rendering. */
void gpr_preview_plan_retain(const gpr_preview_plan *plan);
#ifdef __cplusplus
}
#endif
#endif
