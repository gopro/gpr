/*! @file gpr_read_image.h
 *
 *  @brief Declaration of gpr_read_image class
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

#ifndef GPR_READ_IMAGE
#define GPR_READ_IMAGE

#include "dng_read_image.h"
#include "gpr_buffer_auto.h"

#if GPR_READING

class gpr_read_image : public dng_read_image
{
private:
    gpr_buffer_auto* _vc5_buffer;

    bool fReadVC5;
    
    bool fDecodeVC5;
    
public:
    
    gpr_read_image( gpr_buffer_auto* vc5_buffer = NULL );

    void SetReadVC5(bool read_vc5) { fReadVC5 = read_vc5; }
    bool GetReadVC5() { return fReadVC5; }

    void SetDecodeVC5(bool decode_vc5) { fDecodeVC5 = decode_vc5; }
    bool GetDecodeVC5() { return fDecodeVC5; }

    void ReadTile (dng_host &host,
                   const dng_ifd &ifd,
                   dng_stream &stream,
                   dng_image &image,
                   const dng_rect &tileArea,
                   uint32 plane,
                   uint32 planes,
                   uint32 tileByteCount,
                   AutoPtr<dng_memory_block> &compressedBuffer,
                   AutoPtr<dng_memory_block> &uncompressedBuffer,
                   AutoPtr<dng_memory_block> &subTileBlockBuffer);
};

#endif // GPR_READING

#endif // GPR_READ_IMAGE
