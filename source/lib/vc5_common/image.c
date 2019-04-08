/*! @file image.c
 *
 *  @brief Implementation of the data structure for the image that is
 *  input to the image unpacking process.
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

#include "common.h"

/*!
	@brief Initialize the fields in an image data structure
	This routine is the constructor for the image data type that
	initializes an image instance to default values.
 */
CODEC_ERROR InitImage(IMAGE *image)
{
    if (image != NULL)
    {
        image->width = 0;
        image->height = 0;
        image->pitch = 0;
        image->offset = 0;
        image->format = PIXEL_FORMAT_UNKNOWN;
        image->buffer = NULL;
        image->size = 0;
        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_NULLPTR;
}

CODEC_ERROR InitRGBImage(RGB_IMAGE *image)
{
    if (image != NULL)
    {
        image->width = 0;
        image->height = 0;
        image->pitch = 0;
        image->buffer = NULL;
        image->size = 0;
        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_NULLPTR;
}
/*!
	@brief Allocate the buffer for a image with the sepcified dimensions and format.
	This routine calculates the image pitch from the width and format, rounding up
	the pitch to satisfy memory alignment requirements in the codec.
	This routine will cause a memory leak if it is called with a image that has
	already been allocated.
 */
CODEC_ERROR AllocImage(gpr_allocator *allocator, IMAGE *image, DIMENSION width, DIMENSION height, PIXEL_FORMAT format)
{
    size_t size = 0;
    DIMENSION pitch = 0;
    
    assert(image != NULL);
    
    // Compute the pitch from the width and format
    pitch = ImagePitch(width, format);
    assert(pitch > 0);
    
#if VC5_ENABLED_PART(VC5_PART_COLOR_SAMPLING)
    size = height * pitch;
#else
    size = height * pitch;
#endif
    assert(size > 0);
    
    // Allocate the image buffer
    image->buffer = allocator->Alloc(size);
    if (image->buffer != NULL)
    {
        image->width = width;
        image->height = height;
        image->pitch = pitch;
        image->format = format;
        image->offset = 0;
        image->size = size;
        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_OUTOFMEMORY;
}

/*!
	@brief Deallocate the buffer in a image data structure
	This routine does not deallocate the image data structure itself.
	In the typical use case, the image data structure is allocated on
	the stack and teh decoder allocates the buffer for the output image
	after determining the size of the image.  The image data structure
	is deallocated automatically by a calling routine.
 */
CODEC_ERROR ReleaseImage(gpr_allocator *allocator, IMAGE *image)
{
    allocator->Free(image->buffer);
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Compute the image pitch for the specified width and pixel format
	The pitch of the image (in bytes) is computed for a image with the width
	and pixel format that is specified.  The pitch is rounted up to satisfy
	memory alignment constraints required by the codec.
	The return value is zero if the pitch could not be computed for the
	specified width and format.
 */
DIMENSION ImagePitch(DIMENSION width, PIXEL_FORMAT format)
{
    DIMENSION pitch = 0;
    
    switch (format)
    {
		case PIXEL_FORMAT_RAW_RGGB_12:
		case PIXEL_FORMAT_RAW_RGGB_14:
        case PIXEL_FORMAT_RAW_RGGB_16:
        case PIXEL_FORMAT_RAW_GBRG_12:
		case PIXEL_FORMAT_RAW_GBRG_14:
		case PIXEL_FORMAT_RAW_GBRG_16:
            // Half the width of the image times 2 samples times 2 bytes per sample
            pitch = width * sizeof(uint16_t);
            break;
                    
        default:
            assert(0);
    }
    
    return pitch;
}

/*!
	@brief Set the dimensions and pixel format of a image
	This routine is used to set the dimensions and format of a image
	that was allocated with unknown parameters.
 */
CODEC_ERROR SetImageFormat(IMAGE *image,
                           DIMENSION width,
                           DIMENSION height,
                           DIMENSION pitch,
                           PIXEL_FORMAT format,
                           size_t offset)
{
    assert(image != NULL);
    
    image->width = width;
    image->height = height;
    image->pitch = pitch;
    image->format = format;
    image->offset = offset;
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Return the address of the image in the buffer
	This routine should be used to obtain the starting address of
	a image in a image buffer since the image may be offset from the
	beginning of the buffer.
 */
void *ImageData(IMAGE *image)
{
    uint8_t *buffer = image->buffer;
    buffer += image->offset;
    return buffer;
}

/*!
	@brief Return the address of the specified row in the image
	The routine returns NULL if the image argument is null or the
	specified row is out of bounds.
 */
void *RowAddress(IMAGE *image, DIMENSION row)
{
    size_t pitch = image->pitch;
    
    if (image != NULL && image->pitch != 0)
    {
        //if (0 <= row && row < image->height)
        if (row < image->height)
        {
            void *address = (void *)((uintptr_t)ImageData(image) + row * pitch);
            return address;
        }
    }
    
    // Could not compute the address of a valid row
    return NULL;
}

CODEC_ERROR ReleaseComponentArrays(gpr_allocator *allocator,
                                   UNPACKED_IMAGE *image,
                                   int channel_count )
{
    int channel;
    
    for (channel = 0; channel < channel_count; channel++)
    {
        allocator->Free( image->component_array_list[channel].data );
    }
    
    allocator->Free( image->component_array_list );
    
    return CODEC_ERROR_OKAY;
}

CODEC_ERROR AllocateComponentArrays(gpr_allocator *allocator,
                                    UNPACKED_IMAGE *image,
                                    int channel_count,
                                    DIMENSION max_channel_width,
                                    DIMENSION max_channel_height,
                                    PIXEL_FORMAT format,
                                    int bits_per_component)
{
    int channel;
    
    // Allocate the vector of component arrays
    size_t size = channel_count * sizeof(COMPONENT_ARRAY);
    image->component_array_list = allocator->Alloc(size);
    if (image->component_array_list == NULL) {
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    // Clear the component array information so that the state is consistent
    image->component_count = 0;
    memset(image->component_array_list, 0, size);
    
    // Initialize each component array
    for (channel = 0; channel < channel_count; channel++)
    {
        DIMENSION channel_width = max_channel_width;
        DIMENSION channel_height = max_channel_height;
        
        // Allocate space for the data in the component array
        CODEC_ERROR error = AllocateComponentArray( allocator, &image->component_array_list[channel], channel_width, channel_height, (uint_least8_t)bits_per_component );
        
        if( error != CODEC_ERROR_OKAY ) {
            return error;
        }
    }
    
    // Set the number of component arrays
    image->component_count = channel_count;
    
    return CODEC_ERROR_OKAY;
}

CODEC_ERROR AllocateComponentArray(gpr_allocator *allocator,
                                   COMPONENT_ARRAY *component_array,
                                   DIMENSION width,
                                   DIMENSION height,
                                   PRECISION bits_per_component)
{
    //COMPONENT_ARRAY *component_array = Alloc(allocator, sizeof(COMPONENT_ARRAY));
    
    // Allocate space for the data in the component array
    size_t pitch = width * sizeof(COMPONENT_VALUE);
    size_t size = height * pitch;
    void *buffer = allocator->Alloc(size);
    assert(buffer != NULL);
    if (! (buffer != NULL)) {
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    component_array->width = width;
    component_array->height = height;
    component_array->pitch = pitch;
    component_array->data = buffer;
    component_array->bits_per_component = bits_per_component;
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Initialize the data structure for the unpacked image
	Note that the component arrays will be allocated after the bitstream
	has been decoded and the dimensions of the component arrys are known.
 */
CODEC_ERROR InitUnpackedImage(UNPACKED_IMAGE *unpacked_image)
{
    if (unpacked_image == NULL) {
        return CODEC_ERROR_UNEXPECTED;
    }
    
    // Clear the fields in the unpacked iamge
    memset(unpacked_image, 0, sizeof(UNPACKED_IMAGE));
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Return the maximum number of bits per component across all channels in the unpacked image
 */
PRECISION MaxBitsPerComponent(const UNPACKED_IMAGE *image)
{
	PRECISION max_bits_per_component = 0;
    int channel;
    
    for (channel = 0; channel < image->component_count; channel++)
    {
        COMPONENT_ARRAY *component_array = &image->component_array_list[channel];
        
        if (max_bits_per_component < component_array->bits_per_component) {
            max_bits_per_component = component_array->bits_per_component;
        }
    }
    
    return max_bits_per_component;
}
