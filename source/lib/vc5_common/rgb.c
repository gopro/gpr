/*! @file rgb.c
 *
 *  @brief Implementation of functions that are responsible for
 *  conversion of wavelet to rgb image format
 *
 *  @version 1.0.0
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

#include "common.h"
#include "rgb.h"
#include "tone_curve_acr3.h"

// Black point subtracted in the linear domain (with the range renormalized) before the tone
// curve. The ACR3 curve alone leaves deep shadows slightly gray next to Apple's Core Image
// RAW render, which clips a little more off the bottom; 0.004 matches the CI render's shadow
// percentiles on the HERO13 samples while leaving mid-tones and highlights untouched.
#define PREVIEW_BLACK  0.004f

// Vibrance boost for punchier colors, applied in the gamma-encoded (8-bit) domain. Unlike a
// flat saturation multiplier, vibrance boosts muted colors the most and tapers to no change as
// a pixel approaches full saturation -- so already-vivid colors (and skin tones) are protected
// from clipping/over-saturation. PREVIEW_VIBRANCE is the maximum multiplier, applied to fully
// desaturated pixels; 1.0 = off, larger = more vibrant.
//
// Currently OFF: vibrance was 2.5 when it had to fake saturation to cover for the missing
// camera->sRGB color matrix. With the real matrix applied in WaveletToRGB and the ACR3
// default tone curve in place, the plain render already matches the mean saturation of
// Apple's Core Image RAW render (0.437 vs CI's 0.435 on the HERO13 samples), so any boost
// on top overshoots. Kept (with its guard at the call sites) as the knob to reach for if
// a punchier-than-reference look is ever wanted.
#define PREVIEW_VIBRANCE  1.0f

// Tone mapping + display encoding for a 16-bit linear input, returning a normalized [0,1]
// result. Two steps, matching what DNG renderers (Adobe, Apple's Core Image RAW pipeline)
// do by default:
//
//   1. The ACR3 default tone curve (tone_curve_acr3.h), scene-linear -> display-linear:
//      deep shadow toe, brightened mid-tones, soft highlight shoulder. This replaces the
//      previous hand-rolled pair of a mid-tone lift gamma (1.25) and a Schlick contrast
//      S-curve (1.6), which approximated the same look but grayed out the deepest shadows
//      and left mid-tones darker than the reference renders.
//   2. The sRGB transfer function (linear toe plus a ~2.4 gamma) for display encoding.
//
// Shared by the 8- and 16-bit output paths so both render identically (only the precision
// differs). exposure_gain is the DNG BaselineExposure converted to a linear factor
// (2^EV), applied ahead of everything else exactly as DNG renderers do; highlights that
// a positive gain pushes past 1.0 clip to white.
static float linear16_to_srgb_unit( int linear, float exposure_gain )
{
    float x = ( linear / 65535.0f ) * exposure_gain;

    if( x <= 0.0f )
        return 0.0f;
    if( x >= 1.0f )
        return 1.0f;

    // Black-point clip (see PREVIEW_BLACK above)
    x = ( x - PREVIEW_BLACK ) / ( 1.0f - PREVIEW_BLACK );
    if( x <= 0.0f )
        return 0.0f;

    // ACR3 default tone curve, linearly interpolated between table samples
    {
        float pos   = x * (float)( ACR3_TONE_CURVE_SIZE - 1 );
        int   index = (int)pos;
        float fract = pos - (float)index;

        if( index >= ACR3_TONE_CURVE_SIZE - 1 )
        {
            x = 1.0f;
        }
        else
        {
            x = acr3_tone_curve[index] * ( 1.0f - fract ) + acr3_tone_curve[index + 1] * fract;
        }
    }

    x = ( x <= 0.0031308f ) ? ( 12.92f * x )
                            : ( 1.055f * powf( x, 1.0f / 2.4f ) - 0.055f );

    return x;
}

// Tone-curve lookup tables over the full 16-bit linear domain. linear16_to_srgb_unit costs
// four powf calls, and WaveletToRGB evaluates it three times per output pixel -- by far the
// hottest spot of a GPR->RGB decode. Precomputing all 65536 results (one pass, ~1 ms) turns the
// per-pixel work into an array index. Filled from linear16_to_srgb_unit with the same rounding
// as the direct computation, so the output stays bit-identical.
#define SRGB_LUT_SIZE 65536

static uint8_t  srgb8_lut [SRGB_LUT_SIZE];
static uint16_t srgb16_lut[SRGB_LUT_SIZE];
static float    srgb_luts_gain = -1.0f;   // exposure gain the tables were built for

// Called once per WaveletToRGB invocation. The tables depend on the source's baseline
// exposure, so they are rebuilt (~1 ms) whenever a decode arrives with a different gain
// than the cached fill; back-to-back decodes of same-camera files reuse the cache. The
// decode path is single-threaded (and a concurrent re-fill with the same gain would write
// identical values anyway).
static void init_srgb_luts( float exposure_gain )
{
    int i;

    if( srgb_luts_gain == exposure_gain )
        return;

    for( i = 0; i < SRGB_LUT_SIZE; i++ )
    {
        float unit = linear16_to_srgb_unit( i, exposure_gain );

        srgb8_lut[i]  = (uint8_t) (int)( unit * 255.0f   + 0.5f );
        srgb16_lut[i] = (uint16_t)(int)( unit * 65535.0f + 0.5f );
    }

    srgb_luts_gain = exposure_gain;
}

// White-balance gains can push the linear value past 16 bits; clamping the index to the top
// entry matches the x >= 1 branch of linear16_to_srgb_unit.
static int linear16_to_srgb8( int linear )
{
    int idx = ( linear < 0 ) ? 0 : ( ( linear >= SRGB_LUT_SIZE ) ? SRGB_LUT_SIZE - 1 : linear );
    return srgb8_lut[idx];
}

static int linear16_to_srgb16( int linear )
{
    int idx = ( linear < 0 ) ? 0 : ( ( linear >= SRGB_LUT_SIZE ) ? SRGB_LUT_SIZE - 1 : linear );
    return srgb16_lut[idx];
}

// Vibrance boost for punchier colors: push each channel away from the pixel luma (Rec.601
// weights), scaled by how unsaturated the pixel already is, so vivid colors and skin tones are
// protected. Operates in place on gamma-encoded channels in [0, maxval] (255 or 65535).
static void apply_vibrance( int* R, int* G, int* B, int maxval )
{
    float luma  = *R * 0.299f + *G * 0.587f + *B * 0.114f;
    int   mx    = ( *R > *G ) ? ( *R > *B ? *R : *B ) : ( *G > *B ? *G : *B );
    int   mn    = ( *R < *G ) ? ( *R < *B ? *R : *B ) : ( *G < *B ? *G : *B );
    float sat   = ( mx > 0 ) ? (float)( mx - mn ) / (float)mx : 0.0f;
    float boost = 1.0f + ( PREVIEW_VIBRANCE - 1.0f ) * ( 1.0f - sat );

    int rr = (int)( luma + ( *R - luma ) * boost + 0.5f );
    int gg = (int)( luma + ( *G - luma ) * boost + 0.5f );
    int bb = (int)( luma + ( *B - luma ) * boost + 0.5f );

    *R = ( rr < 0 ) ? 0 : ( ( rr > maxval ) ? maxval : rr );
    *G = ( gg < 0 ) ? 0 : ( ( gg > maxval ) ? maxval : gg );
    *B = ( bb < 0 ) ? 0 : ( ( bb > maxval ) ? maxval : bb );
}

void rgb_parameters_set_default( RGB_PARAMETERS* rgb_params )
{
    int i, j;

    rgb_params->resolution = GPR_RGB_RESOLUTION_DEFAULT;

    rgb_params->bits = 8;

    gpr_rgb_gain_set_defaults( &rgb_params->white_balance_gain );

    rgb_params->black_level = 0;

    for( i = 0; i < 3; i++ )
        for( j = 0; j < 3; j++ )
            rgb_params->color_matrix[i][j] = ( i == j ) ? 1.0f : 0.0f;

    rgb_params->baseline_exposure = 0.0f;

    memset( &rgb_params->shading_map, 0, sizeof(rgb_params->shading_map) );
}

// Bilinear sample of one shading grid at grid coordinates (fv, fh), with edge
// clamping - the same interpolation and clamping dng_gain_map.cpp applies
// (dng_gain_map_interpolator, :123-160).
static float shading_gain( const float* samples, int points_v, int points_h,
                           float fv, float fh )
{
    int   v0, h0, v1, h1;
    float wv, wh, top, bot;

    if( fv < 0.0f ) fv = 0.0f;
    if( fh < 0.0f ) fh = 0.0f;

    v0 = (int)fv;
    h0 = (int)fh;

    if( v0 > points_v - 1 ) v0 = points_v - 1;
    if( h0 > points_h - 1 ) h0 = points_h - 1;

    v1 = ( v0 + 1 < points_v ) ? v0 + 1 : v0;
    h1 = ( h0 + 1 < points_h ) ? h0 + 1 : h0;

    wv = fv - (float)v0;
    wh = fh - (float)h0;

    if( wv < 0.0f ) wv = 0.0f; else if( wv > 1.0f ) wv = 1.0f;
    if( wh < 0.0f ) wh = 0.0f; else if( wh > 1.0f ) wh = 1.0f;

    top = samples[v0 * points_h + h0] * ( 1.0f - wh ) + samples[v0 * points_h + h1] * wh;
    bot = samples[v1 * points_h + h0] * ( 1.0f - wh ) + samples[v1 * points_h + h1] * wh;

    return top * ( 1.0f - wv ) + bot * wv;
}

void WaveletToRGB(gpr_allocator allocator, PIXEL* GS_src, PIXEL* RG_src, PIXEL* BG_src, DIMENSION src_width, DIMENSION src_height, DIMENSION src_pitch,
                  RGB_IMAGE *dst_image, int input_precision_bits, const RGB_PARAMETERS* rgb_params)
{
    TIMESTAMP("[BEG]", 2)

    const int           output_precision_bits = rgb_params->bits;
    const int           black_level           = rgb_params->black_level;
    const gpr_rgb_gain* rgb_gain              = &rgb_params->white_balance_gain;
    const float*        cam_to_srgb           = &rgb_params->color_matrix[0][0];

    init_srgb_luts( exp2f( rgb_params->baseline_exposure ) );

    assert( dst_image );
    assert( dst_image->buffer == NULL );

    // Camera->linear-sRGB color matrix (row-major 3x3), in Q12 fixed point. The white-balance
    // gains alone cannot correct hue: they only scale channels, while the sensor's color
    // filters overlap sRGB primaries, so accurate color needs the cross-channel terms of a
    // full matrix (its negative off-diagonals subtract the filter crosstalk). Identity (the
    // default) keeps the old white-balance-only rendering for callers without a color profile.
    int32_t m[9] = { 4096, 0, 0,  0, 4096, 0,  0, 0, 4096 };
    int     use_matrix = 0;

    {
        int i;
        for( i = 0; i < 9; i++ )
        {
            m[i] = (int32_t)( cam_to_srgb[i] * 4096.0f + ( ( cam_to_srgb[i] >= 0.0f ) ? 0.5f : -0.5f ) );
            use_matrix |= ( m[i] != ( ( i % 4 == 0 ) ? 4096 : 0 ) );
        }
    }
    
    size_t size;
    if( output_precision_bits == 8 )
    {
        size = src_width * src_height * 3;
    }
    else
    {
        size = src_width * src_height * 6;
    }
    
    dst_image->width    = src_width;
    dst_image->height   = src_height;
    dst_image->pitch    = src_width * 3;
    dst_image->size     = size;
    dst_image->buffer   = allocator.Alloc( size );
    
    const int32_t midpoint  = (1 << (input_precision_bits - 1));
    const int32_t shift     = input_precision_bits - 12;
    
    unsigned char*  RGB_dst_8bits  = dst_image->buffer;
    unsigned short* RGB_dst_16bits = dst_image->buffer;

    // Lens shading correction (OpcodeList2 GainMap), applied per pixel below.
    // The grid is addressed in normalized image coordinates, so it is
    // independent of the RGB resolution this render happens to be at - a
    // quarter-size decode samples the same surface as a full-size one.
    const RGB_SHADING_MAP*  shading         = &rgb_params->shading_map;
    const int               shading_active  = ( shading->samples[0] != NULL &&
                                                shading->samples[1] != NULL &&
                                                shading->samples[2] != NULL &&
                                                shading->points_v > 0 && shading->points_h > 0 &&
                                                shading->spacing_v > 0.0f && shading->spacing_h > 0.0f );
    const float             shading_inv_w   = shading_active ? ( 1.0f / (float)src_width  ) : 0.0f;
    const float             shading_inv_h   = shading_active ? ( 1.0f / (float)src_height ) : 0.0f;
    const float             shading_inv_sh  = shading_active ? ( 1.0f / shading->spacing_h ) : 0.0f;
    const float             shading_inv_sv  = shading_active ? ( 1.0f / shading->spacing_v ) : 0.0f;

    DIMENSION x, y;

    for ( y = 0; y < src_height; y++)
    {
        // Grid row coordinate for this output row - constant across the row.
        const float shading_fv = shading_active
            ? ( ( ( (float)y + 0.5f ) * shading_inv_h ) - shading->origin_v ) * shading_inv_sv
            : 0.0f;

        for ( x = 0;  x < src_width; x++)
        {
            int32_t G = GS_src[ x + y * src_pitch];
            int32_t R = 2 * ( RG_src[x + y * src_pitch] - midpoint) + G;
            int32_t B = 2 * ( BG_src[x + y * src_pitch] - midpoint) + G;
            
            // R,G,B are in 16-bit range since DecoderLogCurve outputs in 16 bits (although it's input is 12 bits)
            R = DecoderLogCurve[ clamp_uint( (R >> shift), 12) ];
            G = DecoderLogCurve[ clamp_uint( (G >> shift), 12) ];
            B = DecoderLogCurve[ clamp_uint( (B >> shift), 12) ];

            // Subtract the sensor black level (expressed in this 16-bit linear domain) before
            // applying the white-balance gains. Otherwise the black pedestal is amplified
            // unequally by the per-channel gains and tints the whole image (e.g. magenta for
            // iPhone, whose black level is 528). Cameras with a zero black level (GoPro) are
            // unaffected.
            R = ( R > black_level ) ? ( R - black_level ) : 0;
            G = ( G > black_level ) ? ( G - black_level ) : 0;
            B = ( B > black_level ) ? ( B - black_level ) : 0;

            // Lens shading correction, between black subtraction and white balance -
            // where the DNG spec places OpcodeList2, and where the camera's
            // BaselineExposure assumes it has happened. Without this the corners of a
            // GoPro frame render up to ~1.9 EV dark on every path that renders through
            // here (reduced-size raster exports, the embedded preview, and so the
            // Finder thumbnail and Quick Look preview).
            //
            // Saturated to the 16-bit ceiling: the gains reach ~3.9 at the corners and
            // would otherwise wrap the int32 arithmetic below on a clipped highlight.
            if( shading_active )
            {
                const float fh = ( ( ( (float)x + 0.5f ) * shading_inv_w ) - shading->origin_h ) * shading_inv_sh;

                const float gain_r = shading_gain( shading->samples[0], shading->points_v, shading->points_h, shading_fv, fh );
                const float gain_g = shading_gain( shading->samples[1], shading->points_v, shading->points_h, shading_fv, fh );
                const float gain_b = shading_gain( shading->samples[2], shading->points_v, shading->points_h, shading_fv, fh );

                R = (int32_t)( (float)R * gain_r + 0.5f );
                G = (int32_t)( (float)G * gain_g + 0.5f );
                B = (int32_t)( (float)B * gain_b + 0.5f );

                if( R > 65535 ) R = 65535;
                if( G > 65535 ) G = 65535;
                if( B > 65535 ) B = 65535;
            }

            // Apply the white-balance gains. Common to both output depths -- previously this was
            // only done on the 8-bit path, so 16-bit output was left un-white-balanced (green cast).
            R = ( R * rgb_gain->r_gain_num ) >> rgb_gain->r_gain_pow2_den;
            G = ( G * rgb_gain->g_gain_num ) >> rgb_gain->g_gain_pow2_den;
            B = ( B * rgb_gain->b_gain_num ) >> rgb_gain->b_gain_pow2_den;

            // Camera->sRGB color matrix, applied in the linear domain after white balance (see
            // above). Out-of-gamut negatives are clamped by the tone-curve LUT lookup below.
            if( use_matrix )
            {
                // Saturate to the white point before mixing. The white-balance gains push
                // clipped sensor values past 16 bits carrying no information beyond "white",
                // and feeding those through the matrix's negative terms tints blown
                // highlights (magenta skies). Clamped, a clipped white enters the matrix
                // neutral and leaves it neutral (the rows sum to 1).
                R = ( R > 65535 ) ? 65535 : R;
                G = ( G > 65535 ) ? 65535 : G;
                B = ( B > 65535 ) ? 65535 : B;

                int32_t R2 = (int32_t)( ( (int64_t)m[0] * R + (int64_t)m[1] * G + (int64_t)m[2] * B ) >> 12 );
                int32_t G2 = (int32_t)( ( (int64_t)m[3] * R + (int64_t)m[4] * G + (int64_t)m[5] * B ) >> 12 );
                int32_t B2 = (int32_t)( ( (int64_t)m[6] * R + (int64_t)m[7] * G + (int64_t)m[8] * B ) >> 12 );

                R = R2;
                G = G2;
                B = B2;
            }

            if( output_precision_bits == 8 )
            {
                R = linear16_to_srgb8( R );
                G = linear16_to_srgb8( G );
                B = linear16_to_srgb8( B );

                if( PREVIEW_VIBRANCE != 1.0f ) // constant condition, compiled out when vibrance is off
                    apply_vibrance( &R, &G, &B, 255 );

                RGB_dst_8bits[3 * (x) + 0 + y * dst_image->pitch] = R;
                RGB_dst_8bits[3 * (x) + 1 + y * dst_image->pitch] = G;
                RGB_dst_8bits[3 * (x) + 2 + y * dst_image->pitch] = B;
            }
            else
            {
                R = linear16_to_srgb16( R );
                G = linear16_to_srgb16( G );
                B = linear16_to_srgb16( B );

                if( PREVIEW_VIBRANCE != 1.0f ) // constant condition, compiled out when vibrance is off
                    apply_vibrance( &R, &G, &B, 65535 );

                RGB_dst_16bits[3 * (x) + 0 + y * dst_image->pitch] = ( (R & 0x00FF) << 8 ) | ( (R & 0xFF00) >> 8 );
                RGB_dst_16bits[3 * (x) + 1 + y * dst_image->pitch] = ( (G & 0x00FF) << 8 ) | ( (G & 0xFF00) >> 8 );
                RGB_dst_16bits[3 * (x) + 2 + y * dst_image->pitch] = ( (B & 0x00FF) << 8 ) | ( (B & 0xFF00) >> 8 );
            }
        }
    }
    
    TIMESTAMP("[BEG]", 2)
}
