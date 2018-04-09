/*! @file component.c
 *
 *  @brief Code for parsing the inverse component transform and inverse component permutation.
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

#include "headers.h"

CODEC_ERROR ParseInverseComponentTransform(DECODER *decoder, BITSTREAM *stream, size_t chunk_size)
{
    //CODEC_ERROR error = CODEC_ERROR_OKAY;
    CODEC_STATE *codec = &decoder->codec;
    int component_count = codec->channel_count;
    int padding;
    int i;
    
#if VC5_ENABLED_PART(VC5_PART_COLOR_SAMPLING)
    if (IsPartEnabled(decoder->enabled_parts, VC5_PART_COLOR_SAMPLING))
    {
        // Recompute the number of components to account for color difference component subsampling
        component_count = codec->pattern_width * codec->pattern_height + 2;
    }
#endif
    
    // Compute the padding (in bytes) from the end of the component transform to the end of the chunk payload
    padding = (int)((chunk_size * sizeof(SEGMENT)) - ((component_count + 2) * component_count) * sizeof(uint8_t));
    
    for (i = 0; i < component_count; i++)
    {
        int offset;
        int scale;
        int j;
        
        for (j = 0; j < component_count; j++)
        {
            int matrix_index = i * component_count + j;
            int matrix_value = GetBits(stream, 8);
            
            //TODO: Need to save the value in the codec state
            (void)matrix_index;
            (void)matrix_value;
        }
        
        offset = GetBits(stream, 8);
        scale = GetBits(stream, 8);
        
        //TODO: Need to save the offset and scale in the codec state
        (void)offset;
        (void)scale;
    }
    
    // Skip the padding at the end of the chunk payload
    GetBits(stream, 8 * padding);
    
    // Should be at the end of the last segment in the chunk
    assert(IsAlignedSegment(stream));
    
    return CODEC_ERROR_OKAY;
}

CODEC_ERROR ParseInverseComponentPermutation(DECODER *decoder, BITSTREAM *stream, size_t chunk_size)
{
    //CODEC_ERROR error = CODEC_ERROR_OKAY;
    CODEC_STATE *codec = &decoder->codec;
    int component_count = codec->channel_count;
    int padding;
    int i;
    
#if VC5_ENABLED_PART(VC5_PART_COLOR_SAMPLING)
    if (IsPartEnabled(decoder->enabled_parts, VC5_PART_COLOR_SAMPLING))
    {
        // Recompute the number of components to account for color difference component subsampling
        component_count = codec->pattern_width * codec->pattern_height + 2;
    }
#endif
    
    // Compute the padding (in bytes) from the end of the component transform to the end of the chunk payload
    padding = (int)((chunk_size * sizeof(SEGMENT)) - component_count * sizeof(uint8_t));
    
    for (i = 0; i < component_count; i++)
    {
        int value;
        
        value = GetBits(stream, 8);
        
        //TODO: Need to save the permutation index in yhe codec state
        (void)value;
    }
    
    // Skip the padding at the end of the chunk payload
    GetBits(stream, 8 * padding);
    
    // Should be at the end of the last segment in the chunk
    assert(IsAlignedSegment(stream));
    
    return CODEC_ERROR_OKAY;
}
