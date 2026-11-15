/* SPDX-License-Identifier: Apache-2.0 OR MIT
 * Offline preparation only: use the vendored DNG color specification, including
 * dual illuminants, reference/individual calibration and chromatic adaptation.
 * This tool parses metadata; it does not decode the image or apply a look. */
#include "gpr_platform.h"
#include "dng_auto_ptr.h"
#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_color_spec.h"
#include "dng_file_stream.h"
#include "dng_host.h"
#include "dng_ifd.h"
#include "dng_info.h"
#include "dng_negative.h"
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
int main(int argc,char **argv) {
    try {
        if (argc==2 && !std::strcmp(argv[1],"--version")) {
            std::cout<<"gpr_preview_profile 1; vendored DNG color specification to linear sRGB\n"; return 0;
        }
        if (argc!=2) throw std::runtime_error("usage: gpr_preview_profile source.dng-or-gpr");
        struct stat st={};
        if (stat(argv[1],&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>2ull*1024*1024*1024)
            throw std::runtime_error("requires a regular DNG/GPR of at most 2 GiB");
        dng_host host; host.SetKeepOriginalFile(false);
        dng_file_stream stream(argv[1]); dng_info info;
        info.Parse(host,stream); info.PostParse(host);
        if (!info.IsValidDNG()) throw std::runtime_error("invalid DNG metadata");
        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host,stream,info); negative->PostParse(host,stream,info);
        if (negative->ColorChannels()!=3 || !negative->HasCameraNeutral())
            throw std::runtime_error("requires three camera channels and source AsShotNeutral");
        dng_camera_profile_id profile;
        AutoPtr<dng_color_spec> spec(negative->MakeColorSpec(profile));
        dng_xy_coord white=spec->NeutralToXY(negative->CameraNeutral()); spec->SetWhiteXY(white);
        dng_matrix matrix=dng_space_sRGB::Get().MatrixFromPCS()*spec->CameraToPCS();
        if (matrix.Rows()!=3 || matrix.Cols()!=3 || !std::isfinite(negative->BaselineExposure()))
            throw std::runtime_error("invalid camera color transform");
        for (unsigned y=0;y<3;++y) for (unsigned x=0;x<3;++x)
            if (!std::isfinite(matrix[y][x])) throw std::runtime_error("nonfinite color transform");
        std::cout<<std::setprecision(17)<<"{\"schema\":\"gpr.preview_color.v1\",\"camera_to_srgb\":[";
        for (unsigned y=0;y<3;++y) for (unsigned x=0;x<3;++x)
            std::cout<<(x||y ? ",":"")<<matrix[y][x];
        std::cout<<"],\"camera_white\":[";
        for (unsigned c=0;c<3;++c) std::cout<<(c ? ",":"")<<spec->CameraWhite()[c];
        std::cout<<"],\"white_xy\":["<<white.x<<','<<white.y<<"],\"baseline_exposure_ev\":"<<negative->BaselineExposure()
            <<",\"width\":"<<info.fIFD[info.fMainIndex]->fImageWidth<<",\"height\":"<<info.fIFD[info.fMainIndex]->fImageLength<<"}\n";
        return 0;
    } catch (const std::exception &e) { std::cerr<<"gpr_preview_profile: "<<e.what()<<'\n'; return 1; }
      catch (...) { std::cerr<<"gpr_preview_profile: DNG parsing/color calculation failed\n"; return 1; }
}
