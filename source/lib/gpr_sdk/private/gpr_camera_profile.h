/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_CAMERA_PROFILE_H
#define GPR_CAMERA_PROFILE_H
#include "dng_camera_profile.h"

// The normal SDK setters normalize and round matrices for rendering. When
// rewriting source calibration, retain its numerical scale instead. TIFF
// serialization still uses the DNG writer's 1/10000 rational precision.
class gpr_camera_profile : public dng_camera_profile
{
public:
    void SetSourceMatrices(const dng_matrix &first, const dng_matrix &second)
    {
        fColorMatrix1 = first;
        fColorMatrix2 = second;
        ClearFingerprint();
    }
};
#endif
