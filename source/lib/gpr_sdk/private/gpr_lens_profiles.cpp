/*! @file gpr_lens_profiles.cpp
 *
 *  @brief Built-in geometric lens-distortion profiles for GoPro cameras
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

#include "gpr_lens_profiles.h"

#include <ctype.h>
#include <string.h>

typedef struct
{
    const char* model_substring;    // matched case-insensitively against the EXIF camera model

    double      radial[4];          // k0..k3, same for all planes (DNG WarpRectilinear:
                                    // r_src = r * (k0 + k1*r^2 + k2*r^4 + k3*r^6), r normalized
                                    // by the center-to-corner distance)
    double      center_x;
    double      center_y;

    double      default_strength;   // 0..1 blend toward identity applied by lookup
                                    // unless the caller overrides (see
                                    // gpr_warp_rectilinear_apply_strength)

} gpr_lens_profile;

// Coefficients are full-strength (fully rectilinear) corrections, fitted to the
// DNG polynomial by scripts/fit_warp_rectilinear.py from two kinds of sources:
//
// - HERO5-12: converted (--opencv-fisheye mode) from the Gyroflow lens-profile
//   database, github.com/gyroflow/lens_profiles (CC0-1.0 / public domain).
//   HERO5/6/7 share one official calibration at exactly the 4000x3000 photo
//   size (fx=1755.24, k=[0.04440466,0.01946790,-0.00447670,-0.00204291]), as
//   do HERO9/10 (5312x3984, fx=2373.25, re-centered to the 5568x4176 photo
//   area, same pixel pitch) and HERO11 (5312x4648 8:7, fx=2357.12, re-centered
//   to 5568x4872). HERO12 has the same sensor and lens as HERO11. Independent
//   community calibrations agree with the official ones to <0.4% in focal
//   length, and the implied diagonal FOVs match GoPro's published specs.
// - HERO13: equidistant-fisheye model (r_src = f*atan(r_dst/f), f_norm = 0.74)
//   tuned by rendering samples through a WarpRectilinear-applying DNG pipeline
//   (Apple ImageIO) and straightening door-frame verticals; agrees with the
//   EXIF focal estimate (16 mm equivalent / 21.63 = 0.74). No Gyroflow photo
//   profile exists for HERO13 yet.
// - MISSION 1 PRO: equidistant-fisheye model with f_norm = 0.7546, measured
//   from the camera's own JPEG, which is the uncorrected fisheye at the raw's
//   own 8192x6144 (APP6 LensProjection GPRO, GoPro_LMOD NONE). Subpixel edge
//   tracing of 7 physically straight features - window mullions, rails and
//   casing - over 1380 points spanning r_src 0.14-0.76 drops straightness rms
//   from 21.1 px uncorrected to 3.1 px. Hold-out cross-validation prefers this
//   one-parameter model over free 2/3/4-term polynomials, which diverge across
//   the unmeasured r gap.
//   Corroborated by the camera's own factory calibration, which rides in the
//   JPEG's GoPro APP6 block but in no GPR (the raws carry no GPMF at all):
//   PolynomialCoefficients maps normalized radius to field angle in radians
//   with the frame corner at r = ZoomScaleNormalization, reproducing the
//   stated DiagonalFieldOfView 155.5877 to five decimals. That curve agrees
//   with f_norm 0.7546 to within 1% of source radius everywhere the opcode
//   samples (r_dst 0..1, i.e. r_src 0..0.70) and straightens the same lines to
//   3.1 px, so the two derivations are indistinguishable on this data.
//   The lens is NOT equidistant - the calibration's paraxial f_norm is 0.671
//   against an effective ~0.75 over the correction domain - so 0.7546 is an
//   effective fit across that domain, not a physical focal length. In
//   particular the stated 155.6 degree diagonal must not be read as implying
//   f_norm = 2/155.6deg = 0.7365; that value leaves 3.7 px. HERO13's row is
//   close but measurably worse here (3.5 px), which is what earns this camera
//   its own row. The 50 MP (8192x6144) and 12 MP (4096x3072) photo modes are
//   the same active area binned 2:1, so this one normalized profile covers
//   both.
//
// The calibrated optical centers sit within 0.5% of the frame center; the
// table keeps 0.5/0.5 because DNG opcodes apply in the pre-orientation raw
// frame (GoPro raws are typically mirrored) where a calibrated off-center
// value from an oriented video frame could land on the wrong side.
//
// default_strength blends toward the uncorrected image (1 = fully rectilinear,
// heaviest crop; 0 = no correction): full correction of a ~150 degree fisheye
// pushes the outer ~30% of the image radius off-frame, so the default trades a
// little residual curvature for a much wider kept field of view, similar to
// GoPro's in-camera "Linear" mode.
static const gpr_lens_profile lens_profiles[] =
{
    { "HERO5 Black",  { 0.998988, -0.547459, 0.409714, -0.156401 },  0.5, 0.5,  0.6 },
    { "HERO6 Black",  { 0.998988, -0.547459, 0.409714, -0.156401 },  0.5, 0.5,  0.6 },
    { "HERO7 Black",  { 0.998988, -0.547459, 0.409714, -0.156401 },  0.5, 0.5,  0.6 },
    { "HERO8 Black",  { 0.998614, -0.545837, 0.415701, -0.162626 },  0.5, 0.5,  0.6 },
    { "HERO9 Black",  { 0.998433, -0.583981, 0.465493, -0.186403 },  0.5, 0.5,  0.6 },
    { "HERO10 Black", { 0.998433, -0.583981, 0.465493, -0.186403 },  0.5, 0.5,  0.6 },
    { "HERO11 Black", { 0.997906, -0.653591, 0.554567, -0.227807 },  0.5, 0.5,  0.6 },
    { "HERO12 Black", { 0.997906, -0.653591, 0.554567, -0.227807 },  0.5, 0.5,  0.6 },
    { "HERO13 Black", { 0.999147, -0.573336, 0.417108, -0.153954 },  0.5, 0.5,  0.6 },
    { "MISSION 1 PRO",{ 0.999228, -0.553520, 0.393503, -0.143512 },  0.5, 0.5,  0.6 },
};

// Case-insensitive substring search (strcasestr is not in the C standard).
static int contains_no_case( const char* haystack, const char* needle )
{
    size_t needle_len = strlen( needle );

    for( ; *haystack; haystack++ )
    {
        size_t i;
        for( i = 0; i < needle_len; i++ )
        {
            if( tolower( (unsigned char)haystack[i] ) != tolower( (unsigned char)needle[i] ) )
                break;
        }
        if( i == needle_len )
            return 1;
    }

    return needle_len == 0;
}

int gpr_lens_profile_lookup( const char* camera_model, gpr_warp_rectilinear* out_warp,
                             double* out_default_strength )
{
    if( camera_model == NULL || out_warp == NULL )
        return 0;

    for( size_t i = 0; i < sizeof(lens_profiles) / sizeof(lens_profiles[0]); i++ )
    {
        const gpr_lens_profile& profile = lens_profiles[i];

        if( !contains_no_case( camera_model, profile.model_substring ) )
            continue;

        memset( out_warp, 0, sizeof(*out_warp) );

        out_warp->planes   = 3;
        out_warp->flags    = 0x02;      // mandatory, may be skipped for previews
        out_warp->center_x = profile.center_x;
        out_warp->center_y = profile.center_y;

        for( int p = 0; p < 3; p++ )
        {
            for( int k = 0; k < 4; k++ )
                out_warp->radial[p][k] = profile.radial[k];
        }

        if( out_default_strength != NULL )
            *out_default_strength = profile.default_strength;

        return 1;
    }

    return 0;
}

void gpr_warp_rectilinear_apply_strength( gpr_warp_rectilinear* warp, double strength )
{
    if( !gpr_warp_rectilinear_is_valid( warp ) )
        return;

    for( uint32_t p = 0; p < warp->planes; p++ )
    {
        warp->radial[p][0] = 1.0 + ( warp->radial[p][0] - 1.0 ) * strength;

        for( int k = 1; k < 4; k++ )
            warp->radial[p][k] *= strength;

        warp->tangential[p][0] *= strength;
        warp->tangential[p][1] *= strength;
    }
}

GPR_LENS_PROFILE_RESULT gpr_parameters_apply_lens_warp( gpr_parameters* x,
                                                        const gpr_warp_rectilinear* geo,
                                                        double strength )
{
    if( strength < 0.0 )
        strength = 1.0;
    else if( strength > 1.0 )
        strength = 1.0;

    if( gpr_warp_rectilinear_is_valid( &x->tuning_info.warp ) &&
        !gpr_warp_rectilinear_is_ca_only( &x->tuning_info.warp ) )
    {
        return GPR_LENS_PROFILE_ALREADY_GEOMETRIC;
    }

    // Strength 0 means no geometric correction: keep whatever (CA-only) warp the
    // source carried instead of installing an identity opcode.
    if( strength > 0.0 )
    {
        gpr_warp_rectilinear scaled = *geo;

        gpr_warp_rectilinear_apply_strength( &scaled, strength );

        // Fold a camera-original chromatic aberration warp into the geometric one
        // (after the strength blend: CA is exact optical data, not a matter of
        // correction strength).
        gpr_warp_rectilinear_compose_ca( &scaled, &x->tuning_info.warp );

        x->tuning_info.warp = scaled;
    }

    return GPR_LENS_PROFILE_APPLIED;
}

GPR_LENS_PROFILE_RESULT gpr_parameters_apply_lens_profile( gpr_parameters* x, double strength )
{
    gpr_warp_rectilinear geo;
    double default_strength = 1.0;

    if( !gpr_lens_profile_lookup( x->exif_info.camera_model, &geo, &default_strength ) )
        return GPR_LENS_PROFILE_NOT_FOUND;

    if( strength < 0.0 )
        strength = default_strength;

    return gpr_parameters_apply_lens_warp( x, &geo, strength );
}

void gpr_warp_rectilinear_compose_ca( gpr_warp_rectilinear* geo, const gpr_warp_rectilinear* ca )
{
    if( !gpr_warp_rectilinear_is_valid( geo ) || !gpr_warp_rectilinear_is_ca_only( ca ) )
        return;

    for( uint32_t p = 0; p < geo->planes; p++ )
    {
        // A CA-only warp scales each plane's radius linearly, so it distributes
        // over the geometric polynomial. Single-plane CA scales all planes alike.
        const double scale = ca->radial[ p < ca->planes ? p : 0 ][0];

        for( int k = 0; k < 4; k++ )
            geo->radial[p][k] *= scale;

        geo->tangential[p][0] *= scale;
        geo->tangential[p][1] *= scale;
    }
}
