/*! @file common.h
 *
 *  @brief This file includes all of the header files that are used by
 *  the encoder and decoder. Including a single header file in all
 *  reference encoder and decoder source files ensures that all
 *  modules see the same header files in the same order.
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

#ifndef COMMON_H
#define COMMON_H

#include "gpr_platform.h"
#include "gpr_allocator.h"
#include "gpr_buffer.h"
#include "gpr_rgb_buffer.h"

#include "stdc_includes.h"

#include "log.h"
#include "types.h"
#include "timer.h"
#include "config.h"
#include "macros.h"
#include "error.h"
#include "pixel.h"
#include "image.h"
#include "logcurve.h"
#include "wavelet.h"
#include "bitstream.h"
#include "stream.h"
#include "companding.h"
#include "syntax.h"
#include "codec.h"
#include "codeset.h"
#include "utilities.h"
#include "unique.h"

#endif // COMMON_H
