/*! @file component.c
 *
 *  @brief Implementation of the inverse component transform and inverse component permutation.
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

/*!
    @brief Initialize a component transform
 */
CODEC_ERROR InitComponentTransform(COMPONENT_TRANSFORM *transform)
{
    if (transform != NULL)
    {
        transform->component_count = 0;
        transform->transform_matrix = NULL;
        transform->transform_offset = NULL;
        transform->transform_scale = NULL;
        return CODEC_ERROR_OKAY;
    }
    return CODEC_ERROR_UNEXPECTED;
}

/*!
    @brief Initialize a component permutation
 */
CODEC_ERROR InitComponentPermutation(COMPONENT_PERMUTATION *permutation)
{
    if (permutation != NULL)
    {
        permutation->component_count = 0;
        permutation->permutation_array = NULL;
        return CODEC_ERROR_OKAY;
    }
    return CODEC_ERROR_UNEXPECTED;
}

/*!
    @brief Allocate the arrays in a component transform
 
    The allocated arrays in the component transform are initialized to all zeros.
 */
CODEC_ERROR AllocateComponentTransform(gpr_allocator *allocator, COMPONENT_TRANSFORM *transform, int component_count)
{
    size_t transform_matrix_size = component_count * component_count * sizeof(uint16_t);
    size_t transform_offset_size = component_count * sizeof(uint16_t);
    size_t transform_scale_size = component_count * sizeof(uint16_t);
    
    transform->transform_matrix = (uint16_t *)allocator->Alloc(transform_matrix_size);
    transform->transform_offset = (uint16_t *)allocator->Alloc(transform_offset_size);
    transform->transform_scale = (uint16_t *)allocator->Alloc(transform_scale_size);

    if (transform->transform_matrix == NULL ||
        transform->transform_offset == NULL ||
        transform->transform_scale == NULL) {
        
        //TODO: Should clean up the partially allocated transform arrays
        return CODEC_ERROR_OUTOFMEMORY;
    }

    transform->component_count = component_count;

    memset(transform->transform_matrix, 0, transform_matrix_size);
    memset(transform->transform_offset, 0, transform_offset_size);
    memset(transform->transform_scale, 0, transform_scale_size);
    
    return CODEC_ERROR_OKAY;
}

/*!
    @brief Allocate the arrays in a component permutation
 
    The allocated arrays in the component permutation are initialized to all zeros.
 */
CODEC_ERROR AllocateComponentPermutation(gpr_allocator *allocator, COMPONENT_PERMUTATION *permutation, int component_count)
{
    size_t permutation_array_size = component_count * sizeof(uint16_t);
    
    permutation->permutation_array = (uint16_t *)allocator->Alloc(permutation_array_size);
    if (permutation->permutation_array == NULL) {
        return CODEC_ERROR_OUTOFMEMORY;
    }

    permutation->component_count = component_count;
    
    memset(permutation->permutation_array, 0, permutation_array_size);
    
    return CODEC_ERROR_OKAY;
}

/*!
 @brief Release the arrays in a component transform
 */
CODEC_ERROR ReleaseComponentTransform(gpr_allocator *allocator, COMPONENT_TRANSFORM *transform)
{
    if (transform != NULL)
    {
        allocator->Free(transform->transform_matrix);
        allocator->Free(transform->transform_offset);
        allocator->Free(transform->transform_scale);
        memset(transform, 0, sizeof(COMPONENT_TRANSFORM));
    }
    return CODEC_ERROR_OKAY;
}

/*!
 @brief Release the arrays in a component permutation
 */
CODEC_ERROR ReleaseComponentPermutation(gpr_allocator *allocator, COMPONENT_PERMUTATION *permutation)
{
    if (permutation != NULL)
    {
        allocator->Free(permutation->permutation_array);
        memset(permutation, 0, sizeof(COMPONENT_PERMUTATION));
    }
    return CODEC_ERROR_OKAY;
}

/*!
    @brief Initialize a component transform to the identity transform
 */
CODEC_ERROR InitComponentTransformIdentity(COMPONENT_TRANSFORM *transform, int component_count, gpr_allocator *allocator)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    int component_index;
    
    InitComponentTransform(transform);
    error = AllocateComponentTransform(allocator, transform, component_count);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }

    for (component_index = 0; component_index < component_count; component_index++)
    {
        // Compute the index to the diagonal element in the matrix
        int array_index = component_index * component_count + component_index;
        transform->transform_matrix[array_index] = 1;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
    @brief Initialize a component transform to the identity permutation
 
 */
CODEC_ERROR InitComponentPermutationIdentity(COMPONENT_PERMUTATION *permutation, int component_count, gpr_allocator *allocator)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    int component_index;
    
    InitComponentPermutation(permutation);
    error = AllocateComponentPermutation(allocator, permutation, component_count);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    for (component_index = 0; component_index < component_count; component_index++)
    {
        permutation->permutation_array[component_index] = component_index;
    }
    
    return CODEC_ERROR_OKAY;
}


/*!
 @brief Initialize a component transform to known values for testing
 */
CODEC_ERROR InitComponentTransformTesting(COMPONENT_TRANSFORM *transform, int component_count, gpr_allocator *allocator)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    int row;
    int column;
    
    InitComponentTransform(transform);
    error = AllocateComponentTransform(allocator, transform, component_count);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    for (row = 0; row < component_count; row++)
    {
        for (column = 0; column < component_count; column++)
        {
            // Compute the index to the element in the matrix
            int array_index = row * component_count + column;
            transform->transform_matrix[array_index] = array_index;
        }
   
        transform->transform_offset[row] = (component_count - row);
        transform->transform_scale[row] = row + 1;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
 @brief Initialize a component transform to known values for testing
 
 */
CODEC_ERROR InitComponentPermutationTesting(COMPONENT_PERMUTATION *permutation, int component_count, gpr_allocator *allocator)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    int component_index;
    
    InitComponentPermutation(permutation);
    error = AllocateComponentPermutation(allocator, permutation, component_count);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    for (component_index = 0; component_index < component_count; component_index++)
    {
        permutation->permutation_array[component_index] = component_count - component_index - 1;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
    @brief Return true if the component transform is the identity transform
 */
bool IsComponentTransformIdentity(COMPONENT_TRANSFORM *transform)
{
    int component_count;
    int component_row;
    int component_column;
    
    // A null transform is equivalent to the identity transform
    if (transform == NULL) return true;
    
    component_count = transform->component_count;
    
    for (component_row = 0; component_row < component_count; component_row++)
    {
        // Is the transform matrix the identity matrix?
        for (component_column = 0; component_column < component_count; component_column++)
        {
            // Compute the index to the component element in the transform matrix
            int array_index = component_row * component_count + component_column;
            
            if (component_row == component_column)
            {
                if (transform->transform_matrix[array_index] != 1) return false;
            }
            else
            {
                if (transform->transform_matrix[array_index] != 0) return false;
            }
        }
        
        // Is the transform offset zero?
        if (transform->transform_offset != 0) return false;
        
        // Is the scale factor zero?
        if (transform->transform_scale != 0) return false;
    }

    // The component transform is the identity transform
    return true;
}

/*!
    @brief Allocate the arrays in a component permutation
 */
bool IsComponentPermutationIdentity(COMPONENT_PERMUTATION *permutation)
{
    int component_count;
    int component_index;

    // A null permutation is equivalent to the identity permutation
    if (permutation == NULL) {
        return true;
    }
    
    component_count = permutation->component_count;

    for (component_index = 0; component_index < component_count; component_index++)
    {
        if (permutation->permutation_array[component_index] != component_index) {
            return false;
        }
    }

    // The component permutation is the identity permutation
    return true;
}

/*!
 @brief Write the component transform to the bitstream
 
 @todo Use the InverseTransform16 syntax element if any values are larger than a single byte
 */
CODEC_ERROR WriteComponentTransform(COMPONENT_TRANSFORM *transform, BITSTREAM *stream)
{
    if (transform != NULL)
    {
        const int component_count = transform->component_count;
        const size_t chunk_payload_size = (component_count * component_count + 2 * component_count) * sizeof(uint8_t);
        const size_t chunk_payload_padding = sizeof(SEGMENT) - (chunk_payload_size % sizeof(SEGMENT));
        const int chunk_payload_length = (int)((chunk_payload_size + sizeof(SEGMENT) - 1) / sizeof(SEGMENT));
        int i;
        
        // Write the tag value pair for the small chunk element for the component transform
        PutTagPair(stream, CODEC_TAG_InverseTransform, chunk_payload_length);
        
        for (i = 0; i < component_count; i++)
        {
            int offset_value;
            int scale_value;
            
            // Write the row at this index in the transform matrix
            int j;
            
            for (j = 0; j < component_count; j++)
            {
                int array_index = i * component_count + j;
                int array_value = transform->transform_matrix[array_index];
                
                assert(INT8_MIN <= array_value && array_value <= INT8_MAX);
                PutBits(stream, array_value, 8);
            }
            
            // Write the offset
            offset_value = transform->transform_offset[i];
            assert(INT8_MIN <= offset_value && offset_value <= INT8_MAX);
            PutBits(stream, offset_value, 8);
            
            // Write the scale
            scale_value = transform->transform_scale[i];
            assert(0 <= scale_value && scale_value <= UINT8_MAX);
            PutBits(stream, scale_value, 8);
        }
        
        // Pad the remainer of the chunk payload with zeros
        for (i = 0; i < (int)chunk_payload_padding; i++) {
            PutBits(stream, 0, 8);
        }
        
        // Check that the bitstream is aligned on a segment boundary
        assert(IsAlignedSegment(stream));
        if (! (IsAlignedSegment(stream))) {
            return CODEC_ERROR_UNEXPECTED;
        }
        
        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_UNEXPECTED;
}

/*!
 @brief Write the component permutation to the bitstream
 */
CODEC_ERROR WriteComponentPermutation(COMPONENT_PERMUTATION *permutation, BITSTREAM *stream)
{
    if (permutation != NULL)
    {
        const int component_count = permutation->component_count;
        const size_t chunk_payload_size = component_count * sizeof(uint8_t);
        const size_t chunk_payload_padding = sizeof(SEGMENT) - (chunk_payload_size % sizeof(SEGMENT));
        const int chunk_payload_length = (int)((chunk_payload_size + sizeof(SEGMENT) - 1) / sizeof(SEGMENT));
        int i;
        
        // Write the tag value pair for the small chunk element for the component transform
        PutTagPair(stream, CODEC_TAG_InversePermutation, chunk_payload_length);
        
        for (i = 0; i < component_count; i++)
        {
            uint8_t value = (uint8_t)permutation->permutation_array[i];
            assert(0 <= value && value <= UINT8_MAX);
            PutBits(stream, value, 8);
        }
        
        // Pad the remainer of the chunk payload with zeros
        for (i = 0; i < (int)chunk_payload_padding; i++) {
            PutBits(stream, 0, 8);
        }
        
        // Check that the bitstream is aligned on a segment boundary
        assert(IsAlignedSegment(stream));
        if (! (IsAlignedSegment(stream))) {
            return CODEC_ERROR_UNEXPECTED;
        }
        
        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_UNEXPECTED;
}
