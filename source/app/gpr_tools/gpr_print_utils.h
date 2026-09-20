/*! @file gpr_print_utils.h
 *
 *  @brief Printing utilities for gpr_tools
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

#ifndef GPR_PRINT_UTILS_H
#define GPR_PRINT_UTILS_H

#include "gpr.h"

#ifdef __cplusplus
extern "C" {
#endif
    
    //!< Write gpr_parameters as JSON, to output_file_path or to stdout when it is NULL
    //!< (readable back with gpr_parameters_parse_json).
    int gpr_parameters_print_json( const gpr_parameters* parameters, const char* output_file_path );
    
#ifdef __cplusplus
}
#endif

#endif // GPR_PRINT_UTILS_H
