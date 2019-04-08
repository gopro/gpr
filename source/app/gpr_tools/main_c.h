/*! @file main_c.h
 *
 *  @brief Definition of C conversion routines used by gpr_tools
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

#ifndef MAIN_C_H
#define MAIN_C_H

#ifdef __cplusplus
extern "C" {
#endif

    int dng_convert_main(const char*  input_file_path, unsigned int input_width, unsigned int input_height, size_t input_pitch, size_t input_skip_rows, size_t input_header_skip, const char* input_pixel_format,
                         const char*  output_file_path, const char*  exiftool_file_path, const char* gpmf_file_path, const char* rgb_file_resolution, int rgb_file_bits,
                         const char*  jpg_preview_file_path, int jpg_preview_file_width, int jpg_preview_file_height );
    
#ifdef __cplusplus
}
#endif

#endif // MAIN_C_H
