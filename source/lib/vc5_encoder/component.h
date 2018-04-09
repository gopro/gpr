/*! @file component.h
 *
 *  @brief Declaration of routines for the inverse component transform and permutation.
 
 *  A codebooks contains the variable-length codes for coefficient magnitudes, runs
 *  of zeros, and special codewords that mark entropy codec locations in bitstream.
 
 *  The codebook is used to create tables and simplify entropy coding of signed values
 *  and runs of zeros.
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

#ifndef COMPONENT_H
#define COMPONENT_H

/*!
 @brief Data structure for the component transform (16 bit precision)
 */
typedef struct _component_transform
{
    uint16_t component_count;
    uint16_t *transform_matrix;
    uint16_t *transform_offset;
    uint16_t *transform_scale;
    
} COMPONENT_TRANSFORM;

/*!
 @brief Data structure for the component permutation
 */
typedef struct _component_permutation
{
    uint16_t component_count;
    uint16_t *permutation_array;
    
} COMPONENT_PERMUTATION;

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR InitComponentTransform(COMPONENT_TRANSFORM *transform);

    CODEC_ERROR InitComponentPermutation(COMPONENT_PERMUTATION *permutation);
    
    CODEC_ERROR AllocateComponentTransform(gpr_allocator *allocator,
                                           COMPONENT_TRANSFORM *transform,
                                           int component_count);

    CODEC_ERROR AllocateComponentPermutation(gpr_allocator *allocator,
                                             COMPONENT_PERMUTATION *permutation,
                                             int component_count);

    CODEC_ERROR ReleaseComponentTransform(gpr_allocator *allocator,
                                          COMPONENT_TRANSFORM *transform);
    
    CODEC_ERROR ReleaseComponentPermutation(gpr_allocator *allocator,
                                            COMPONENT_PERMUTATION *permutation);
    
    CODEC_ERROR InitComponentTransformIdentity(COMPONENT_TRANSFORM *transform,
                                               int component_count,
                                               gpr_allocator *allocator);

    CODEC_ERROR InitComponentPermutationIdentity(COMPONENT_PERMUTATION *permutation,
                                                 int component_count,
                                                 gpr_allocator *allocator);
    
    CODEC_ERROR InitComponentTransformTesting(COMPONENT_TRANSFORM *transform,
                                              int component_count,
                                              gpr_allocator *allocator);

    CODEC_ERROR InitComponentPermutationTesting(COMPONENT_PERMUTATION *permutation,
                                                int component_count,
                                                gpr_allocator *allocator);

    bool IsComponentTransformIdentity(COMPONENT_TRANSFORM *transform);

    bool IsComponentPermutationIdentity(COMPONENT_PERMUTATION *permutation);

    CODEC_ERROR WriteComponentTransform(COMPONENT_TRANSFORM *transform, BITSTREAM *stream);

    CODEC_ERROR WriteComponentPermutation(COMPONENT_PERMUTATION *permutation, BITSTREAM *stream);

#ifdef __cplusplus
}
#endif

#endif // COMPONENT_H
