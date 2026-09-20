/*! @file gpr_rgb.c
 *
 *  @brief Implementation of top level functions that implement GPR-SDK API
 *
 *  GPR API can be invoked by simply including this header file.
 *  This file includes all other header files that are needed.
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

#include "gpr.h"
#include "gpr_rgb.h"
#include "stdc_includes.h"

// Matrix helpers below feed write_dng, which is compiled unconditionally because the
// DNG conversion API (gpr_convert_raw_to_dng and friends) does not depend on the VC5
// codec paths. They must stay outside the GPR_READING/GPR_WRITING guard.

static void multiply_3x3( const double a[3][3], const double b[3][3], double out[3][3] )
{
    int i, j;
    for ( i = 0; i < 3; i++ )
        for ( j = 0; j < 3; j++ )
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

/*!
 * 3x3 inverse via the adjugate. Returns false (out untouched) for a singular matrix.
 */
static bool invert_3x3( const double a[3][3], double inv[3][3] )
{
    double det = a[0][0] * ( a[1][1] * a[2][2] - a[1][2] * a[2][1] )
               - a[0][1] * ( a[1][0] * a[2][2] - a[1][2] * a[2][0] )
               + a[0][2] * ( a[1][0] * a[2][1] - a[1][1] * a[2][0] );
    if ( fabs( det ) < 1e-9 )
        return false;

    inv[0][0] =  ( a[1][1] * a[2][2] - a[1][2] * a[2][1] ) / det;
    inv[0][1] = -( a[0][1] * a[2][2] - a[0][2] * a[2][1] ) / det;
    inv[0][2] =  ( a[0][1] * a[1][2] - a[0][2] * a[1][1] ) / det;
    inv[1][0] = -( a[1][0] * a[2][2] - a[1][2] * a[2][0] ) / det;
    inv[1][1] =  ( a[0][0] * a[2][2] - a[0][2] * a[2][0] ) / det;
    inv[1][2] = -( a[0][0] * a[1][2] - a[0][2] * a[1][0] ) / det;
    inv[2][0] =  ( a[1][0] * a[2][1] - a[1][1] * a[2][0] ) / det;
    inv[2][1] = -( a[0][0] * a[2][1] - a[0][1] * a[2][0] ) / det;
    inv[2][2] =  ( a[0][0] * a[1][1] - a[0][1] * a[1][0] ) / det;
    return true;
}

void compute_xyz_to_camera_color_matrix( double in_matrix[3][3], double wb[3], double weight, double out_matrix[3][3] )
{
    double temp1[3][3];
    double temp2[3][3];

    int i,j;

    // Interpolate with identity matrix by weight w
    double w = weight;
    double z = 1.0 - weight;

    for (i = 0; i < 3; i++ )
    {
        for (j = 0; j < 3; j++ )
            temp1[i][j] = in_matrix[i][j] * w;

        temp1[i][i] += z;
    }

    // Multiply matrix by sRGB_to_XYZd50 (from http://www.brucelindbloom.com)
    static const double sRGB_to_XYZd50[3][3] = {{0.4361, 0.3851, 0.1431}, {0.2225, 0.7169, 0.0606}, {0.0139, 0.0971, 0.7142}};

    multiply_3x3( sRGB_to_XYZd50, temp1, temp2 );

    // Multiply on the right by diag(wb), i.e. scale column j by white balance gain j
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            temp1[i][j] = temp2[i][j] * wb[j];
    
    // Invert the resulting matrix. A singular matrix (degenerate profile or zero wb
    // gains) falls back to identity rather than emitting NaN/Inf into the DNG tags.
    if ( !invert_3x3( temp1, out_matrix ) )
    {
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                out_matrix[i][j] = ( i == j ) ? 1.0 : 0.0;
    }
}

// Color-rendering helpers, shared by the RGB decode path (gpr_convert_gpr_to_rgb) and
// the encode path (which bakes the same rendering into the embedded preview/thumbnail).
#if GPR_READING || GPR_WRITING

/*!
 * Correlated color temperature (Kelvin) of the EXIF CalibrationIlluminant codes GoPro
 * profiles use; 0 for codes we do not recognize (callers substitute the DNG-conventional
 * pairing of Standard A / D65).
 */
static double illuminant_cct( uint16_t code )
{
    switch ( code )
    {
        case 3:  return 2856.0;   // Tungsten
        case 17: return 2856.0;   // Standard light A
        case 18: return 4874.0;   // Standard light B
        case 19: return 6774.0;   // Standard light C
        case 20: return 5503.0;   // D55
        case 21: return 6504.0;   // D65
        case 22: return 7504.0;   // D75
        case 23: return 5003.0;   // D50
        case 24: return 3200.0;   // ISO studio tungsten
        default: return 0.0;
    }
}

/*!
 * Camera -> linear-sRGB color matrix for the RGB output paths (GPR decode, and the
 * preview embedded at encode), derived the way DNG
 * renderers do it: interpolate the profile's two ColorMatrix calibrations (XYZ -> camera)
 * to the shot's white point, invert to get camera -> XYZ, convert XYZ to linear sRGB, and
 * fold in the white-balance gains that WaveletToRGB has already applied by the time the
 * matrix runs. Each row is then normalized to sum to 1 so a white-balanced neutral stays
 * neutral -- the matrix corrects hue and saturation only, leaving exposure and white
 * balance untouched. Writes identity and returns false when the source has no usable
 * profile (WaveletToRGB then renders exactly as before).
 *
 * The interpolation weight comes from the DNG spec's self-consistent loop: guess a
 * weight, blend the matrices, map the as-shot neutral (the inverse of the WB gains)
 * through the blend to an XYZ white point, estimate its color temperature (McCamy), and
 * re-derive the weight by inverse-CCT (mired) between the two calibration illuminants,
 * until stable. Daylight shots land near ColorMatrix2 (D65); tungsten shots near
 * ColorMatrix1 (Standard A).
 */
bool compute_camera_to_srgb_color_matrix( const gpr_tuning_info* tuning_info, const gpr_profile_info* profile_info, float out_matrix[3][3] )
{
    int i, j;

    for ( i = 0; i < 3; i++ )
        for ( j = 0; j < 3; j++ )
            out_matrix[i][j] = ( i == j ) ? 1.0f : 0.0f;

    const float gains[3] = { tuning_info->wb_gains.r_gain,
                             tuning_info->wb_gains.g_gain,
                             tuning_info->wb_gains.b_gain };
    if ( gains[0] < 0.01f || gains[1] < 0.01f || gains[2] < 0.01f )
        return false;

    // The as-shot neutral in camera space (the white balance undone), used both to
    // estimate the shot's color temperature and as the white point the Bradford
    // adaptation below maps to D65.
    const double neutral[3] = { 1.0 / gains[0], 1.0 / gains[1], 1.0 / gains[2] };

    const double (*cm1)[3] = profile_info->color_matrix_1;
    const double (*cm2)[3] = profile_info->color_matrix_2;

    double magnitude1 = 0.0, magnitude2 = 0.0;
    for ( i = 0; i < 3; i++ )
    {
        for ( j = 0; j < 3; j++ )
        {
            magnitude1 += fabs( cm1[i][j] );
            magnitude2 += fabs( cm2[i][j] );
        }
    }
    if ( magnitude1 < 1e-6 && magnitude2 < 1e-6 )
        return false;

    // Blend weight toward ColorMatrix1: 0 = pure ColorMatrix2 (daylight), 1 = pure
    // ColorMatrix1 (tungsten). With only one usable matrix there is nothing to blend.
    double weight = 0.0;
    if ( magnitude1 < 1e-6 )
        weight = 0.0;
    else if ( magnitude2 < 1e-6 )
        weight = 1.0;
    else
    {
        double cct1 = illuminant_cct( profile_info->illuminant1 );
        double cct2 = illuminant_cct( profile_info->illuminant2 );
        if ( cct1 <= 0.0 ) cct1 = 2856.0;   // DNG convention: illuminant 1 = Standard A
        if ( cct2 <= 0.0 ) cct2 = 6504.0;   //                 illuminant 2 = D65
        if ( fabs( cct1 - cct2 ) < 1.0 )
        {
            weight = 0.0;
        }
        else
        {
            int iteration;
            for ( iteration = 0; iteration < 20; iteration++ )
            {
                double blend[3][3], blend_inv[3][3];
                for ( i = 0; i < 3; i++ )
                    for ( j = 0; j < 3; j++ )
                        blend[i][j] = weight * cm1[i][j] + ( 1.0 - weight ) * cm2[i][j];
                if ( !invert_3x3( blend, blend_inv ) )
                    break;

                double X = blend_inv[0][0] * neutral[0] + blend_inv[0][1] * neutral[1] + blend_inv[0][2] * neutral[2];
                double Y = blend_inv[1][0] * neutral[0] + blend_inv[1][1] * neutral[1] + blend_inv[1][2] * neutral[2];
                double Z = blend_inv[2][0] * neutral[0] + blend_inv[2][1] * neutral[1] + blend_inv[2][2] * neutral[2];
                double sum = X + Y + Z;
                if ( sum <= 1e-9 )
                    break;

                // McCamy's chromaticity-to-CCT approximation, good within the 2000-10000K
                // range the calibrations span.
                double x = X / sum;
                double y = Y / sum;
                if ( fabs( 0.1858 - y ) < 1e-6 )
                    break;
                double t   = ( x - 0.3320 ) / ( 0.1858 - y );
                double cct = 449.0 * t * t * t + 3525.0 * t * t + 6823.3 * t + 5520.33;
                if ( cct < 1500.0 )  cct = 1500.0;
                if ( cct > 50000.0 ) cct = 50000.0;

                double next = ( 1.0 / cct - 1.0 / cct2 ) / ( 1.0 / cct1 - 1.0 / cct2 );
                if ( next < 0.0 ) next = 0.0;
                if ( next > 1.0 ) next = 1.0;

                if ( fabs( next - weight ) < 1e-4 )
                {
                    weight = next;
                    break;
                }
                weight = next;
            }
        }
    }

    double xyz_to_cam[3][3];
    for ( i = 0; i < 3; i++ )
        for ( j = 0; j < 3; j++ )
            xyz_to_cam[i][j] = weight * cm1[i][j] + ( 1.0 - weight ) * cm2[i][j];

    // Invert XYZ -> camera into camera -> XYZ
    double inv[3][3];
    if ( !invert_3x3( xyz_to_cam, inv ) )
        return false;

    // Bradford chromatic adaptation from the shot's white point to D65. The camera->XYZ
    // matrix above lands colors in XYZ relative to the scene illuminant; sRGB is defined
    // for D65. Previously the gap was papered over by normalizing each output row
    // independently, which keeps neutrals neutral but distorts non-neutral colors by a
    // different amount per channel -- a warm cast on indoor shots was the visible result.
    // The Bradford transform is the DNG-prescribed way: scale the white point's cone-space
    // (LMS-like) responses onto D65's, which carries all colors along consistently.
    double adapt[3][3] = { { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, { 0.0, 0.0, 1.0 } };
    {
        static const double bradford[3][3] =
        {
            {  0.8951,  0.2664, -0.1614 },
            { -0.7502,  1.7135,  0.0367 },
            {  0.0389, -0.0685,  1.0296 },
        };
        static const double bradford_inv[3][3] =
        {
            {  0.9869929, -0.1470543,  0.1599627 },
            {  0.4323053,  0.5183603,  0.0492912 },
            { -0.0085287,  0.0400428,  0.9684867 },
        };
        static const double d65_white[3] = { 0.95047, 1.0, 1.08883 };

        // The shot's white point: the as-shot neutral mapped through camera -> XYZ
        double white[3];
        for ( i = 0; i < 3; i++ )
            white[i] = inv[i][0] * neutral[0] + inv[i][1] * neutral[1] + inv[i][2] * neutral[2];

        if ( white[0] > 1e-9 && white[1] > 1e-9 && white[2] > 1e-9 )
        {
            double cone_white[3], cone_d65[3];
            for ( i = 0; i < 3; i++ )
            {
                cone_white[i] = bradford[i][0] * white[0] / white[1]
                              + bradford[i][1]
                              + bradford[i][2] * white[2] / white[1];
                cone_d65[i]   = bradford[i][0] * d65_white[0]
                              + bradford[i][1] * d65_white[1]
                              + bradford[i][2] * d65_white[2];
            }
            if ( cone_white[0] > 1e-9 && cone_white[1] > 1e-9 && cone_white[2] > 1e-9 )
            {
                // adapt = bradford_inv * diag(cone_d65 / cone_white) * bradford
                double scaled[3][3];
                for ( i = 0; i < 3; i++ )
                    for ( j = 0; j < 3; j++ )
                        scaled[i][j] = ( cone_d65[i] / cone_white[i] ) * bradford[i][j];
                multiply_3x3( bradford_inv, scaled, adapt );
            }
        }
    }

    // XYZ (D65) -> linear sRGB, the standard sRGB primaries matrix
    static const double xyz_to_srgb[3][3] =
    {
        {  3.2404542, -1.5371385, -0.4985314 },
        { -0.9692660,  1.8760108,  0.0415560 },
        {  0.0556434, -0.2040259,  1.0572252 },
    };

    // S = XYZ->sRGB * adapt * camera->XYZ, then fold in the white balance: WaveletToRGB
    // applies the matrix AFTER the wb gains, so divide column j by gain j to operate on
    // pre-gain values.
    double adapted[3][3], full[3][3];
    multiply_3x3( adapt, inv, adapted );
    multiply_3x3( xyz_to_srgb, adapted, full );

    double composed[3][3];
    for ( i = 0; i < 3; i++ )
        for ( j = 0; j < 3; j++ )
            composed[i][j] = full[i][j] / gains[j];

    // Row-normalize so a white-balanced neutral (R=G=B) maps to itself exactly. With the
    // Bradford adaptation in place the rows already sum to ~1 by construction (the
    // adaptation maps the shot's white point onto D65, which the sRGB matrix maps to
    // R=G=B); this only mops up the rounding of the published matrix constants, where it
    // used to shoulder the whole adaptation. Normalized into a local first so a
    // degenerate row leaves out_matrix a clean identity rather than half-written.
    float normalized[3][3];
    for ( i = 0; i < 3; i++ )
    {
        double row_sum = composed[i][0] + composed[i][1] + composed[i][2];
        if ( row_sum < 1e-6 )
            return false;
        for ( j = 0; j < 3; j++ )
            normalized[i][j] = (float)( composed[i][j] / row_sum );
    }

    memcpy( out_matrix, normalized, sizeof(normalized) );
    return true;
}

#endif // GPR_READING || GPR_WRITING
