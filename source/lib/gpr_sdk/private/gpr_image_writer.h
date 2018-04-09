/*! @file gpr_image_writer.h
 *
 *  @brief Declaration of gpr_image_writer class
 *
 *  GPR API can be invoked by simply including this header file.
 *  This file includes all other header files that are needed.
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

#ifndef GPR_IMAGE_WRITER
#define GPR_IMAGE_WRITER

#include "dng_image_writer.h"
#include "gpr_buffer_auto.h"

#if GPR_WRITING

#include "vc5_encoder.h"

class gpr_image_writer : public dng_image_writer
{
private:
    
    gpr_buffer_auto     _vc5_buffer_obj;
    
    gpr_rgb_buffer      _rgb_thumbnail;

            gpr_buffer_auto*   _vc5_buffer;
    const   gpr_buffer_auto*   _raw_buffer;
    
    vc5_encoder_parameters vc5_encoder_params;
    
public:
    
    void EncodeVc5Image();
    
    const gpr_rgb_buffer& get_rgb_thumbnail() { return _rgb_thumbnail; }
    
    vc5_encoder_parameters& GetVc5EncoderParams() { return vc5_encoder_params; }
    
    gpr_image_writer(const gpr_buffer_auto* raw_buffer,
                         unsigned int raw_buffer_width,
                         unsigned int raw_buffer_height,
                         size_t       raw_buffer_pitch,
                         gpr_buffer_auto* vc5_buffer = NULL );
    
    ~gpr_image_writer();
    
    void WriteImage (dng_host &host,
                     const dng_ifd &ifd,
                     dng_basic_tag_set &basic,
                     dng_stream &stream,
                     const dng_image &image,
                     uint32 fakeChannels);
    
    uint32 CompressedBufferSize (const dng_ifd &ifd, uint32 uncompressedSize);
    
    uint32 GetDefaultCompression() { return ccVc5; }
    
    void WriteTile (dng_host &host,
                    const dng_ifd &ifd,
                    dng_stream &stream,
                    const dng_image &image,
                    const dng_rect &tileArea,
                    uint32 fakeChannels,
                    AutoPtr<dng_memory_block> &compressedBuffer,
                    AutoPtr<dng_memory_block> &uncompressedBuffer,
                    AutoPtr<dng_memory_block> &subTileBlockBuffer,
                    AutoPtr<dng_memory_block> &tempBuffer);
    
};

#endif // GPR_WRITING

#endif // GPR_IMAGE_WRITER
