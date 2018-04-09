/*! @file headers.h
 *
 *  @brief This file includes all of the header files that are used by the encoder.
 *
 *  Note that some header files are only used by the main program that
 *  calls the codec or are only used for debugging are not included by this file.
 *  Only headers that are part of the reference encoder are included by this file.
 *
 *  Including a single header file in all reference encoder source files
 *  ensures that all modules see the same header files in the same order.
 *
 *  This file can be used for creating a pre-compiled header if the
 *  compiler supports that capabilities.
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

#ifndef HEADERS_H
#define HEADERS_H

#include "common.h"

#include "vlc.h"
#include "bitstream.h"
#include "raw.h"
#include "codebooks.h"
#include "component.h"
#include "syntax.h"
#include "forward.h"

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
#include "sections.h"
#endif

#include "parameters.h"
#include "encoder.h"
#include "vc5_encoder.h"

#endif // HEADERS_H
