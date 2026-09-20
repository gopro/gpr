/*! @file dng_stage1_negative.cpp
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

#include "dng_stage1_negative.h"

#include "dng_auto_ptr.h"
#include "dng_exceptions.h"
#include "dng_host.h"

dng_stage1_negative::dng_stage1_negative( dng_host &host ) : dng_negative( host ) { }

dng_stage1_negative * dng_stage1_negative::Make( dng_host &host )
{
    AutoPtr<dng_stage1_negative> result( new dng_stage1_negative( host ) );

    if( !result.Get() )
    {
        ThrowMemoryFull();
    }

    result->Initialize();

    return result.Release();
}

dng_image * dng_stage1_negative::DetachStage1Image() { return fStage1Image.Release(); }
