/*! @file dng_stage1_negative.h
 *
 *  @brief dng_negative subclass that exposes ownership of its stage-1 image
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

#ifndef DNG_STAGE1_NEGATIVE_H
#define DNG_STAGE1_NEGATIVE_H

#include "dng_negative.h"

class dng_host;
class dng_image;

// dng_negative offers no way to take ownership of its stage-1 image (Stage1Image() is
// const-only), but its constructor, Initialize() and fStage1Image are all protected, so a
// subclass can expose a detach. The caller must construct the negative itself, which is what
// makes a static_cast at the detach site well-defined.
class dng_stage1_negative : public dng_negative
{
public:
    explicit dng_stage1_negative( dng_host &host );

    static dng_stage1_negative * Make( dng_host &host );

    dng_image * DetachStage1Image();
};

#endif // DNG_STAGE1_NEGATIVE_H
