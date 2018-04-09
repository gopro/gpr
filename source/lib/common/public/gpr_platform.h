/*! @file gpr_platform.h
 *
 *  @brief Definitions of platform related settings
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

#ifndef GPR_PLATFORM_H
#define GPR_PLATFORM_H

// =================================================================================================
// Timing output. Define to:
// 0 for applications (disabled all timing code)
// 1 to enable top level timing
// 2 to enable low level timing
// =================================================================================================

#ifndef GPR_TIMING
    #define GPR_TIMING 1
#endif

// =================================================================================================
// Defines to enable encoder and decoder API in GPR-SDK
// =================================================================================================

#ifndef GPR_WRITING
    #define GPR_WRITING         1
#endif

#ifndef GPR_READING
    #define GPR_READING         1
#endif

#ifndef GPR_JPEG_AVAILABLE
    #define GPR_JPEG_AVAILABLE  1
#endif

// =================================================================================================
// Flags to enable/disable SIMD code
// =================================================================================================

// Neon code is implemented for encoder
#ifndef NEON
    #define NEON 0
#endif

// SSE code is not implemented yet
#ifndef SSE
    #define SSE 0
#endif

// =================================================================================================
// Do not change lines below
// =================================================================================================

// =================================================================================================
// Common header files needed by GPR-SDK
// =================================================================================================
#include <stdlib.h>
#include <stdint.h>

#ifndef float_t
typedef float float_t;
#endif

#ifdef _WIN32

// Turn off warnings about deprecated functions
#pragma warning(disable: 4996)

#define INLINE __inline

#else

#define INLINE inline

#endif // _WIN32

#define STATIC static

#define STATIC_INLINE STATIC INLINE

#define ENABLED(x) (x)

// =================================================================================================
// Determine the Platform
// =================================================================================================

#ifdef _WIN32

    #define qWinOS      1
    #define qMacOS      0
    #define qLinux      0
    #define qiPhone     0
    #define qAndroid    0

#elif __APPLE__

    #include "TargetConditionals.h"

    #define qEnableCarbon   0 // Disable Carbon API because it is old and deprecated by Apple

    #if TARGET_OS_IPHONE

        #define qWinOS      0
        #define qMacOS      0
        #define qLinux      0
        #define qiPhone     1
        #define qAndroid    0

    #else

        #define qWinOS      0
        #define qMacOS      1
        #define qLinux      0
        #define qiPhone     0
        #define qAndroid    0

    #endif

#elif __ANDROID__

    #define qWinOS      0
    #define qMacOS      0
    #define qLinux      0
    #define qiPhone     0
    #define qAndroid    1

#elif __linux__ || __unix__

    #define qWinOS      0
    #define qMacOS      0
    #define qLinux      1
    #define qiPhone     0
    #define qAndroid    0

#else
    #error "XMP environment error - Unknown compiler"
#endif

// =================================================================================================
// GPR version numbering
// =================================================================================================

#define GPR_VERSION_MAJOR        1
#define GPR_VERSION_MINOR        0
#define GPR_VERSION_REVISION     0

#endif // GPR_PLATFORM_H
