/*! @file image.h
 *
 *  @brief Declaration of structures and functions for images
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

#ifndef IMAGE_H
#define IMAGE_H

//! Data type for the values in a component array
typedef uint16_t COMPONENT_VALUE;

/*!
	@brief Data structure for an image input to the unpacking process

	This data structure is used to represent the image that is the input to the
	image unpacking process that unpacks an image into component arrays for encoding.
	Unlike the wavelet data structures, an image contains multiple color components,
	usually in a packed pixel format.
*/
typedef struct _packed_image
{
	DIMENSION width;			//!< Width of the frame (in pixels)
	DIMENSION height;			//!< Height of the frame
	size_t pitch;				//!< Distance between rows (in bytes)
	PIXEL_FORMAT format;		//!< Format of the pixels
	void *buffer;				//!< Address of the buffer for the frame
	size_t size;				//!< Allocated size of the buffer (in bytes)
	size_t offset;				//!< Offset to the start of the frame    
} PACKED_IMAGE;

/*!
 @brief Data structure for an image input to the unpacking process
 
 This data structure is used to represent the image that is the input to the
 image unpacking process that unpacks an image into component arrays for encoding.
 Unlike the wavelet data structures, an image contains multiple color components,
 usually in a packed pixel format.
 */
typedef struct _rgb_image
{
    DIMENSION width;            //!< Width of the frame (in pixels)
    DIMENSION height;            //!< Height of the frame
    size_t pitch;                //!< Distance between rows (in bytes)
    void *buffer;                //!< Address of the buffer for the frame
    size_t size;                //!< Allocated size of the buffer (in bytes)
} RGB_IMAGE;

//! Short name for the packed image data type
typedef PACKED_IMAGE IMAGE;

/*!
	@brief Data structure for an array that contains a single type of component

	This data structure is used to represent the component array output by the image
	unpacking process.  The image unpacking process unpacks an image into component
	arrays for encoding.
*/
typedef struct _component_array
{
	DIMENSION width;			//!< Width of the frame (in pixels)
	DIMENSION height;			//!< Height of the frame
	size_t pitch;				//!< Distance between rows (in bytes)
	COMPONENT_VALUE *data;		//!< Buffer for the array of component values

	//! Number of bits per in each component value
	PRECISION bits_per_component;

} COMPONENT_ARRAY;

/*!
	@brief Image represented as an ordered set of component arrays

	The decoder outputs a set of component arrays that represent an image.

	The image repacking process can pack the component arrays output by the
	decoder into a packed image.
*/
typedef struct _unpacked_image
{
	//! Number of component arrays in the unpacked image
	int component_count;

	//! Vector of component arrays
	COMPONENT_ARRAY *component_array_list;

} UNPACKED_IMAGE;

/*!
	@brief Flags that describe the image structure
*/
typedef enum
{
	IMAGE_STRUCTURE_INTERLACED = 0x0001,			//!< Set the first bit if the image is interlaced
	IMAGE_STRUCTURE_BOTTOM_FIELD_FIRST = 0x0002,	//!< The bottom field is encoded before the top field
	IMAGE_STRUCTURE_BOTTOM_ROW_FIRST = 0x0010,		//!< The encoded image is upside down
} IMAGE_STRUCTURE;


#ifdef __cplusplus
extern "C" {
#endif

    CODEC_ERROR InitImage(IMAGE *image);
    
    CODEC_ERROR InitRGBImage(RGB_IMAGE *image);
    
    CODEC_ERROR AllocImage(gpr_allocator *allocator, IMAGE *image, DIMENSION width, DIMENSION height, PIXEL_FORMAT format);
    
    CODEC_ERROR ReleaseImage(gpr_allocator *allocator, IMAGE *image);
    
    DIMENSION ImagePitch(DIMENSION width, PIXEL_FORMAT format);
    
    CODEC_ERROR SetImageFormat(IMAGE *image,
                               DIMENSION width,
                               DIMENSION height,
                               DIMENSION pitch,
                               PIXEL_FORMAT format,
                               size_t offset);
    
    void *ImageData(IMAGE *image);
    
    void *RowAddress(IMAGE *image, DIMENSION row);
    
    CODEC_ERROR ReleaseComponentArrays(gpr_allocator *allocator,
                                       UNPACKED_IMAGE *image,
                                       int channel_count );
    
    CODEC_ERROR AllocateComponentArrays(gpr_allocator *allocator,
                                        UNPACKED_IMAGE *image,
                                        int channel_count,
                                        DIMENSION max_channel_width,
                                        DIMENSION max_channel_height,
                                        PIXEL_FORMAT format,
                                        int bits_per_component);
    
    CODEC_ERROR AllocateComponentArray(gpr_allocator *allocator,
                                       COMPONENT_ARRAY *component_array,
                                       DIMENSION width,
                                       DIMENSION height,
                                       PRECISION bits_per_component);
    
    CODEC_ERROR InitUnpackedImage(UNPACKED_IMAGE *image);
    
    PRECISION MaxBitsPerComponent(const UNPACKED_IMAGE *image);
    
#ifdef __cplusplus
}
#endif

#endif // IMAGE_H
