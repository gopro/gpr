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

// Mid-tone lift applied (in the linear domain) before sRGB encoding. It is a power curve that
// brightens shadows and mid-tones while keeping pure black and pure white fixed, approximating
// the tone lift that finished renders (e.g. the iPhone camera pipeline) apply. Without it a
// faithful linear->sRGB render of a dim scene looks crushed/dark. >1 brightens.
#define PREVIEW_MIDTONE_LIFT_GAMMA  1.25f

// Contrast S-curve strength, applied to the gamma-encoded value around a mid-gray pivot of 0.5.
// DNG renderers (Adobe, macOS Preview) apply a contrasty default tone curve on top of the raw
// data, so without an equivalent curve here the embedded preview looks flat/washed-out next to
// the rendered DNG. Uses the Schlick bias/gain curve x^c / (x^c + (1-x)^c), which keeps black,
// mid-gray and white fixed while darkening shadows and brightening highlights. 1.0 = off,
// larger = more contrast.
#define PREVIEW_CONTRAST  1.6f

// Vibrance boost for punchier colors, applied in the gamma-encoded (8-bit) domain. Unlike a
// flat saturation multiplier, vibrance boosts muted colors the most and tapers to no change as
// a pixel approaches full saturation -- so already-vivid colors (and skin tones) are protected
// from clipping/over-saturation. PREVIEW_VIBRANCE is the maximum multiplier, applied to fully
// desaturated pixels; 1.0 = off, larger = more vibrant.
#define PREVIEW_VIBRANCE  2.5f

// sRGB transfer function (including the mid-tone lift above) for a 16-bit linear input,
// returning a normalized [0,1] result. This replaces the previous plain sqrt() approximation of
// display gamma; the sRGB curve (linear toe plus a ~2.4 gamma) is more accurate, and the lift
// keeps dim scenes from rendering too dark in the preview/RGB output. Shared by the 8- and
// 16-bit output paths so both render identically (only the precision differs).
static float linear16_to_srgb_unit( int linear )
{
    float x = linear / 65535.0f;

    if( x <= 0.0f )
        return 0.0f;
    if( x >= 1.0f )
        return 1.0f;

    x = powf( x, 1.0f / PREVIEW_MIDTONE_LIFT_GAMMA );

    x = ( x <= 0.0031308f ) ? ( 12.92f * x )
                            : ( 1.055f * powf( x, 1.0f / 2.4f ) - 0.055f );

    // Contrast S-curve (see PREVIEW_CONTRAST above), applied to the gamma-encoded value so the
    // pivot sits at perceptual mid-gray.
    {
        float xc  = powf( x, PREVIEW_CONTRAST );
        float ixc = powf( 1.0f - x, PREVIEW_CONTRAST );
        x = xc / ( xc + ixc );
    }

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
static int      srgb_luts_ready = 0;

// Called once per WaveletToRGB invocation (the decode path is single-threaded; a concurrent
// re-init would be harmless anyway since every fill writes the same values).
static void init_srgb_luts( void )
{
    int i;

    if( srgb_luts_ready )
        return;

    for( i = 0; i < SRGB_LUT_SIZE; i++ )
    {
        float unit = linear16_to_srgb_unit( i );

        srgb8_lut[i]  = (uint8_t) (int)( unit * 255.0f   + 0.5f );
        srgb16_lut[i] = (uint16_t)(int)( unit * 65535.0f + 0.5f );
    }

    srgb_luts_ready = 1;
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

void WaveletToRGB(gpr_allocator allocator, PIXEL* GS_src, PIXEL* RG_src, PIXEL* BG_src, DIMENSION src_width, DIMENSION src_height, DIMENSION src_pitch,
                  RGB_IMAGE *dst_image, int input_precision_bits, int output_precision_bits, int black_level, gpr_rgb_gain* rgb_gain)
{
    TIMESTAMP("[BEG]", 2)

    init_srgb_luts();

    assert( dst_image );
    assert( dst_image->buffer == NULL );
    
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
    
    DIMENSION x, y;
    
    for ( y = 0; y < src_height; y++)
    {
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

            // Apply the white-balance gains. Common to both output depths -- previously this was
            // only done on the 8-bit path, so 16-bit output was left un-white-balanced (green cast).
            R = ( R * rgb_gain->r_gain_num ) >> rgb_gain->r_gain_pow2_den;
            G = ( G * rgb_gain->g_gain_num ) >> rgb_gain->g_gain_pow2_den;
            B = ( B * rgb_gain->b_gain_num ) >> rgb_gain->b_gain_pow2_den;

            if( output_precision_bits == 8 )
            {
                R = linear16_to_srgb8( R );
                G = linear16_to_srgb8( G );
                B = linear16_to_srgb8( B );

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

                apply_vibrance( &R, &G, &B, 65535 );

                RGB_dst_16bits[3 * (x) + 0 + y * dst_image->pitch] = ( (R & 0x00FF) << 8 ) | ( (R & 0xFF00) >> 8 );
                RGB_dst_16bits[3 * (x) + 1 + y * dst_image->pitch] = ( (G & 0x00FF) << 8 ) | ( (G & 0xFF00) >> 8 );
                RGB_dst_16bits[3 * (x) + 2 + y * dst_image->pitch] = ( (B & 0x00FF) << 8 ) | ( (B & 0xFF00) >> 8 );
            }
        }
    }
    
    TIMESTAMP("[BEG]", 2)
}
