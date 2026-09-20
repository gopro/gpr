/*! @file gpr_lens_profiles.h
 *
 *  @brief Built-in geometric lens-distortion profiles for GoPro cameras
 *
 *  GPR files carry no geometric distortion metadata (at most a chromatic
 *  aberration WarpRectilinear on older cameras), so a DNG that should render
 *  distortion-corrected needs a synthesized OpcodeList3 WarpRectilinear.
 *  This module maps a camera model string to such a warp.
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

#ifndef GPR_LENS_PROFILES_H
#define GPR_LENS_PROFILES_H

#include "gpr.h"

#ifdef __cplusplus
    extern "C" {
#endif

    typedef enum
    {
        GPR_LENS_PROFILE_APPLIED = 0,               // tuning_info.warp now carries the geometric correction

        GPR_LENS_PROFILE_NOT_FOUND = 1,             // no built-in profile for the camera model; nothing changed

        GPR_LENS_PROFILE_ALREADY_GEOMETRIC = 2,     // source already carries a geometric (non-CA-only) warp,
                                                    // which was kept untouched to avoid double correction

    } GPR_LENS_PROFILE_RESULT;

    // Looks up the built-in geometric distortion profile for a camera model string
    // (EXIF camera model, e.g. "HERO6 Black"; matched case-insensitively as a
    // substring). Returns 1 and fills out_warp when a profile exists, 0 otherwise.
    // out_warp holds the full-strength (fully rectilinear) correction; the
    // profile's recommended strength is stored to *out_default_strength (may be
    // NULL) and must be applied by the caller via
    // gpr_warp_rectilinear_apply_strength.
    int gpr_lens_profile_lookup( const char* camera_model, gpr_warp_rectilinear* out_warp,
                                 double* out_default_strength );

    // Blends a geometric warp toward the identity (no correction): 1 leaves it
    // unchanged (fully rectilinear, heaviest crop), 0 makes it a no-op (full
    // fisheye field of view kept). The blend is exact: the correction curve
    // interpolates linearly in the polynomial coefficients.
    void gpr_warp_rectilinear_apply_strength( gpr_warp_rectilinear* warp, double strength );

    // Folds a camera-original chromatic aberration warp (per-plane linear scale,
    // see gpr_warp_rectilinear_is_ca_only) into a geometric profile by scaling each
    // plane's coefficients, so a single opcode carries both corrections.
    void gpr_warp_rectilinear_compose_ca( gpr_warp_rectilinear* geo, const gpr_warp_rectilinear* ca );

    // One-call lens correction for parsed gpr_parameters (the same behavior as
    // gpr_tools --lens_correction=auto): looks up the built-in profile for
    // x->exif_info.camera_model, blends it to the given strength, folds in a
    // camera-original CA-only warp, and stores the result in x->tuning_info.warp
    // so the next DNG write carries the OpcodeList3 WarpRectilinear.
    //
    // strength: 0..1 (clamped); 1 = fully rectilinear (heaviest crop), 0 = leave
    // the image uncorrected (x is not modified). Pass a negative value to use the
    // profile's recommended strength.
    GPR_LENS_PROFILE_RESULT gpr_parameters_apply_lens_profile( gpr_parameters* x, double strength );

    // Same, but with caller-supplied full-strength geometric coefficients instead
    // of a profile lookup (gpr_tools --lens_correction=k0,k1,k2,k3). A negative
    // strength means 1 here (coefficients used as given). Never returns
    // GPR_LENS_PROFILE_NOT_FOUND.
    GPR_LENS_PROFILE_RESULT gpr_parameters_apply_lens_warp( gpr_parameters* x,
                                                            const gpr_warp_rectilinear* geo,
                                                            double strength );

#ifdef __cplusplus
    }
#endif

#endif // GPR_LENS_PROFILES_H
