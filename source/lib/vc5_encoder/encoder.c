/*! @file encoder.c
 *
 *  @brief Implementation of functions for encoding samples.
 *
 *  Encoded samples must be aligned on a four byte boundary.
 *  Any constraints on the alignment of data within the sample
 *  are handled by padding the sample to the correct alignment.
 *
 *  Note that the encoded dimensions are the actual dimensions of each channel
 *  (or the first channel in the case of 4:2:2 sampling) in the encoded sample.
 *  The display offsets and dimensions specify the portion of the encoded frame
 *  that should be displayed, but in the case of a Bayer image the display
 *  dimensions are doubled to account for the effects of the demosaic filter.
 *  If a Bayer image is encoded to Bayer format (no demosaic filter applied),
 *  then the encoded dimensions will be the same as grid of Bayer quads, less
 *  any padding required during encoding, but the display dimensions and
 *  offset will be reported as if a demosiac filter were applied to scale the
 *  encoded frame to the display dimensions (doubling the width and height).
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

#if ENABLED(NEON)
#include <arm_neon.h>
#endif

/*!
	@brief Align the bitstream to a byte boundary
 
	Enough bits are written to the bitstream to align the
	bitstream to the next byte.
 */
static CODEC_ERROR AlignBitsByte(BITSTREAM *bitstream)
{
    if (bitstream->count > 0 && (bitstream->count % 8) != 0)
    {
        // Compute the number of bits of padding
        BITCOUNT count = (8 - (bitstream->count % 8));
        PutBits(bitstream, 0, count);
    }
    assert((bitstream->count % 8) == 0);
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Align the bitstream to the next segment
 
	The corresponding function in the existing codec flushes the bitstream.
 
	@todo Is it necessary to flush the bitstream (and the associated byte stream)
	after aligning the bitstream to a segment boundary?
 */
static CODEC_ERROR AlignBitsSegment(BITSTREAM *bitstream)
{
    STREAM *stream = bitstream->stream;
    size_t byte_count;
    
    // Byte align the bitstream
    AlignBitsByte(bitstream);
    assert((bitstream->count % 8) == 0);
    
    // Compute the number of bytes in the bit buffer
    byte_count = bitstream->count / 8;
    
    // Add the number of bytes written to the stream
    byte_count += stream->byte_count;
    
    while ((byte_count % sizeof(TAGVALUE)) != 0)
    {
        PutBits(bitstream, 0, 8);
        byte_count++;
    }
    
    // The bitstream should be aligned to the next segment
    assert((bitstream->count == 0) || (bitstream->count == bit_word_count));
    assert((byte_count % sizeof(TAGVALUE)) == 0);
    
    return CODEC_ERROR_OKAY;
}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
/*!
	@brief Set default values for the pattern element structure

	Some image formats imply specific parameters for the dimensions of the
	pattern elements and the number of components per sample.  If the pattern
	element structure has not been fully specified by the command-line
	arguments, then missing values can be filled in from the default values
	for the image format.
 */
bool SetImageFormatDefaults(ENCODER *encoder)
{
	switch (encoder->image_format)
	{
#if VC5_ENABLED_PART(VC5_PART_COLOR_SAMPLING)
        if (IsPartEnabled(encoder->enabled_parts, VC5_PART_COLOR_SAMPLING))
        {
            // The components per sample parameter is not applicable to VC-5 Part 4 bitstreams
            assert(encoder->components_per_sample == 0);
            encoder->components_per_sample = 0;
        }
        else
        {
            // Set the default components per sample assuming no alpha channel
            if (encoder->components_per_sample == 0) {
                encoder->components_per_sample = 3;
            }
        }
#else
		// Set the default components per sample assuming no alpha channel
		if (encoder->components_per_sample == 0) {
			encoder->components_per_sample = 3;
		}
#endif
		return true;

    case IMAGE_FORMAT_RAW:
        if (encoder->pattern_width == 0) {
            encoder->pattern_width = 2;
        }
            
        if (encoder->pattern_height == 0) {
            encoder->pattern_height = 2;
        }
            
        if (encoder->components_per_sample == 0) {
            encoder->components_per_sample = 1;
        }
            
		return true;

	default:
		// Unable to set default values for the pattern elements
		return false;
	}
}
#endif

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
/*!
	@brief Check for inconsistent values for the parameters specified on the command-line

	This routine looks for inconsistencies between the image format, the dimensions of the
	pattern elements, and the number of components per sample.
 */
bool CheckImageFormatParameters(ENCODER *encoder)
{
	switch (encoder->image_format)
	{            
    case IMAGE_FORMAT_RAW:
            
        if (encoder->pattern_width != 2) {
            return false;
        }
            
        if (encoder->pattern_height != 2) {
            return false;
        }
            
        if (encoder->components_per_sample != 1) {
            return false;
        }
            
        // The parameters for the Bayer image format are correct
        return true;
            
	default:
		// Cannot verify the parameters for an unknown image format
		return false;

	}
}
#endif

/*!
	@brief Prepare the encoder state
*/
CODEC_ERROR PrepareEncoderState(ENCODER *encoder,
								const UNPACKED_IMAGE *image,
								const ENCODER_PARAMETERS *parameters)
{
	CODEC_STATE *codec = &encoder->codec;
	int channel_count = image->component_count;
	int channel_number;

	// Set the default value for the number of bits per lowpass coefficient
	PRECISION lowpass_precision = 16;

	if (parameters->encoded.lowpass_precision > 0) {
		lowpass_precision = parameters->encoded.lowpass_precision;
	}

	for (channel_number = 0; channel_number < channel_count; channel_number++)
	{
		DIMENSION width = image->component_array_list[channel_number].width;
		DIMENSION height = image->component_array_list[channel_number].height;
		PRECISION bits_per_component = image->component_array_list[channel_number].bits_per_component;

		// Copy the component array parameters into the encoder state
		encoder->channel[channel_number].width = width;
		encoder->channel[channel_number].height = height;
		encoder->channel[channel_number].bits_per_component = bits_per_component;

		// The lowpass bands in all channels are encoded with the same precision
		encoder->channel[channel_number].lowpass_precision = lowpass_precision;
	}

	// Record the number of channels in the encoder state
	encoder->channel_count = channel_count;

	// The encoder uses three wavelet transform levels for each channel
	encoder->wavelet_count = 3;

	// Set the channel encoding order
	if (parameters->channel_order_count > 0)
	{
		// Use the channel order specified by the encoding parameters
		encoder->channel_order_count = parameters->channel_order_count;
		memcpy(encoder->channel_order_table, parameters->channel_order_table, sizeof(encoder->channel_order_table));
	}
	else
	{
		// Use the default channel encoding order
		for (channel_number = 0; channel_number < channel_count; channel_number++)
		{
			encoder->channel_order_table[channel_number] = channel_number;
		}
		encoder->channel_order_count = channel_count;
	}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	// The actual image dimensions are reported in the bitstream header (VC-5 Part 3)
	encoder->image_width = parameters->input.width;
	encoder->image_height = parameters->input.height;
	encoder->pattern_width = parameters->pattern_width;
	encoder->pattern_height = parameters->pattern_height;
	encoder->components_per_sample = parameters->components_per_sample;
	encoder->image_format = parameters->encoded.format;
    encoder->max_bits_per_component = MaxBitsPerComponent(image);

	// Set default parameters for the image format
	SetImageFormatDefaults(encoder);

	if (!CheckImageFormatParameters(encoder)) {
		return CODEC_ERROR_BAD_IMAGE_FORMAT;
	}
#else
	// The dimensions of the image is the maximum of the channel dimensions (VC-5 Part 1)
	GetMaximumChannelDimensions(image, &encoder->image_width, &encoder->image_height);
#endif

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	// Interlaced images are encoded as separate layers
	encoder->progressive = parameters->progressive;
    encoder->top_field_first = TRUE;
    encoder->frame_inverted = FALSE;
	encoder->progressive = 1;

	// Set the number of layers (sub-samples) in the encoded sample
	encoder->layer_count = parameters->layer_count;
#endif

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    // by default, all sections are enabled
    encoder->enabled_sections = parameters->enabled_sections;
#endif
    
	// Initialize the codec state with the default parameters used by the decoding process
	return PrepareCodecState(codec);
}

/*!
	@brief Initialize the encoder data structure

	This routine performs the same function as a C++ constructor.
	The encoder is initialized with default values that are replaced
	by the parameters used to prepare the encoder (see @ref PrepareEncoder).

	This routine does not perform all of the initializations required
	to prepare the encoder data structure for decoding a sample.
*/
CODEC_ERROR InitEncoder(ENCODER *encoder, const gpr_allocator *allocator, const VERSION *version)
{
	assert(encoder != NULL);
	if (! (encoder != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	memset(encoder, 0, sizeof(ENCODER));

	// Assign a memory allocator to the encoder
	encoder->allocator = (gpr_allocator *)allocator;

	// Write debugging information to standard output
	encoder->logfile = stdout;

	if (version)
	{
		// Store the version number in the encoder
		memcpy(&encoder->version, version, sizeof(encoder->version));
	}
	else
	{
		// Clear the version number in the encoder
		memset(&encoder->version, 0, sizeof(encoder->version));
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Encode the image into the output stream

	This is a convenience routine for applications that use a byte stream to
	represent a memory buffer or binary file that will store the encoded image.

	The image is unpacked into a set of component arrays by the image unpacking
	process invoked by calling the routine @ref ImageUnpackingProcess.  The image
	unpacking process is informative and is not part of the VC-5 standard.

	The main entry point for encoding the component arrays output by the image
	unpacking process is @ref EncodingProcess.
*/
CODEC_ERROR EncodeImage(IMAGE *image, STREAM *stream, RGB_IMAGE *rgb_image, ENCODER_PARAMETERS *parameters)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;

	// Allocate data structures for the encoder state and the bitstream
	ENCODER encoder;
	BITSTREAM bitstream;
    
    SetupEncoderLogCurve();
    
	UNPACKED_IMAGE unpacked_image;

	// Unpack the image into a set of component arrays
	error = ImageUnpackingProcess(image, &unpacked_image, parameters, &parameters->allocator);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Initialize the bitstream data structure
	InitBitstream(&bitstream);

	// Bind the bitstream to the byte stream
	error = AttachBitstream(&bitstream, stream);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Encode the component arrays into the bitstream
	error = EncodingProcess(&encoder, &unpacked_image, &bitstream, parameters);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}
    
    if( rgb_image != NULL && parameters->rgb_resolution == GPR_RGB_RESOLUTION_SIXTEENTH )
    { // Thumbnail
        SetupDecoderLogCurve();

        WaveletToRGB(parameters->allocator,
                     encoder.transform[0].wavelet[2]->data[LL_BAND], encoder.transform[1].wavelet[2]->data[LL_BAND], encoder.transform[2].wavelet[2]->data[LL_BAND],
                     encoder.transform[0].wavelet[2]->width, encoder.transform[0].wavelet[2]->height, encoder.transform[0].wavelet[2]->width,
                     rgb_image, 14, 8, &parameters->rgb_gain );
    }
    
    error = ReleaseComponentArrays( &parameters->allocator, &unpacked_image, unpacked_image.component_count );
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
	// Release any resources allocated by the bitstream
	ReleaseBitstream(&bitstream);

	// Release any resources allocated by the encoder
	ReleaseEncoder(&encoder);
    
	return error;
}

/*!
	@brief Reference implementation of the VC-5 encoding process.

	The encoder takes input image in the form of a list of component arrays
	produced by the image unpacking process and encodes the image into the
	bitstream.

	External parameters are used to initialize the encoder state.

	The encoder state determines how the image is encoded int the bitstream.
*/
CODEC_ERROR EncodingProcess(ENCODER *encoder,
							const UNPACKED_IMAGE *image,
							BITSTREAM *bitstream,
							const ENCODER_PARAMETERS *parameters)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;

	// Initialize the encoder using the parameters provided by the application
	error = PrepareEncoder(encoder, image, parameters);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	if (encoder->image_format == IMAGE_FORMAT_UNKNOWN) {
		return CODEC_ERROR_BAD_IMAGE_FORMAT;
	}
	if ( parameters->verbose_flag )
	{
		LogPrint("Pattern width: %d\n", encoder->pattern_width);
		LogPrint("Pattern height: %d\n", encoder->pattern_height);
        
        if (!IsPartEnabled(encoder->enabled_parts, VC5_PART_COLOR_SAMPLING)) {
            LogPrint("Components per sample: %d\n", encoder->components_per_sample);
        }
        LogPrint("Internal precision: %d\n", encoder->internal_precision);
        
		LogPrint("\n");
	}
#endif
    
	// Write the bitstream start marker
	PutBitstreamStartMarker(bitstream);

    // Allocate six pairs of lowpass and highpass buffers for each channel
    AllocateEncoderHorizontalBuffers(encoder);
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)

    if (!IsPartEnabled(encoder.enabled_parts, VC5_PART_LAYERS) || encoder.layer_count == 1)
    {
        // Encode the image as a single layer in the sample
        error = EncodeSingleImage(encoder, ImageData(image), image->pitch, &bitstream);
    }
	else
	{
		// Each layer encodes a separate frame
		IMAGE *image_array[MAX_LAYER_COUNT];
		memset(image_array, 0, sizeof(image_array));

		// The encoding parameters must include a decompositor
		assert (parameters->decompositor != NULL);

		// Decompose the frame into individual frames for each layer
		error = parameters->decompositor(image, image_array, encoder.layer_count);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}

		// Encode each frame as a separate layer in the sample
		error = EncodeMultipleImages(encoder, image_array, encoder.layer_count, &bitstream);
    }
#else

    // Encode one image into the bitstream
	error = EncodeSingleImage(encoder, image, bitstream);

#endif

    DeallocateEncoderHorizontalBuffers(encoder);

	return error;
}

/*!
	@brief Initialize the encoder using the specified parameters

	It is important to use the correct encoded image dimensions (including padding)
	and the correct encoded format to initialize the encoder.  The decoded image
	dimensions must be adjusted to account for a lower decoded resolution if applicable.

	It is expected that the parameters data structure may change over time
	with additional or different fields, depending on the codec profile or
	changes made to the codec during further development.  The parameters
	data structure may have a version number or may evolve into a dictionary
	of key-value pairs with missing keys indicating that a default value
	should be used.

	@todo Add more error checking to this top-level routine
*/
CODEC_ERROR PrepareEncoder(ENCODER *encoder,
						   const UNPACKED_IMAGE *image,
						   const ENCODER_PARAMETERS *parameters)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
	VERSION version = VERSION_INITIALIZER(VC5_VERSION_MAJOR, VC5_VERSION_MINOR, VC5_VERSION_REVISION, 0);
    PRECISION max_bits_per_component = MaxBitsPerComponent(image);

	// Initialize the encoder data structure
	InitEncoder(encoder, &parameters->allocator, &version);

	// Set the mask that specifies which parts of the VC-5 standard are supported
	encoder->enabled_parts = parameters->enabled_parts;

	// Verify that the enabled parts are correct
	error = VerifyEnabledParts(encoder->enabled_parts);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Remember the internal precision used by the image unpacking process
    encoder->internal_precision = minimum(max_bits_per_component, default_internal_precision);
    
	// Initialize the encoding parameters and the codec state
	PrepareEncoderState(encoder, image, parameters);

	// Allocate the wavelet transforms
	AllocEncoderTransforms(encoder);

	// Initialize the quantizer
	SetEncoderQuantization(encoder, parameters);

	// Initialize the wavelet transforms
	PrepareEncoderTransforms(encoder);

	// Allocate the scratch buffers used for encoding
	AllocEncoderBuffers(encoder);

	// Initialize the encoding tables for magnitudes and runs of zeros
	error = PrepareCodebooks(&parameters->allocator, &encoder_codeset_17 );
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Select the codebook for encoding
	encoder->codeset = &encoder_codeset_17;

    // The encoder is ready to decode a sample
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Free all resources allocated by the encoder
*/
CODEC_ERROR ReleaseEncoder(ENCODER *encoder)
{
	if (encoder != NULL)
	{
		gpr_allocator *allocator = encoder->allocator;
		int channel;

		// Free the encoding tables
		ReleaseCodebooks(allocator, encoder->codeset);

		// Free the wavelet tree for each channel
		for (channel = 0; channel < MAX_CHANNEL_COUNT; channel++)
		{
			ReleaseTransform(allocator, &encoder->transform[channel]);
		}

		//TODO: Free the encoding buffers
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Encode a single image into the bitstream

	This is the main entry point for encoding a single image into the bitstream.
	The encoder must have been initialized by a call to @ref PrepareEncoder.

	The unpacked image is the set of component arrays output by the image unpacking
	process.  The bitstream must be initialized and bound to a byte stream before
	calling this routine.
*/
CODEC_ERROR EncodeSingleImage(ENCODER *encoder, const UNPACKED_IMAGE *image, BITSTREAM *stream)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;

	// Write the sample header that is common to all layers
	error = EncodeBitstreamHeader(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Write the sample extension header to the bitstream
	error = EncodeExtensionHeader(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Encode each component array as a separate channel in the bitstream
	error = EncodeMultipleChannels(encoder, image, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Finish the encoded sample after the last layer
	error = EncodeBitstreamTrailer(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Force any data remaining in the bitstream to be written into the sample
	FlushBitstream(stream);
    
	return error;
}

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Encode multiple frames as separate layers in a sample

	The encoder must have been initialized by a call to @ref PrepareEncoder.

	The bitstream must be initialized and bound to a byte stream before
	calling this routine.
*/
CODEC_ERROR EncodeMultipleFrames(ENCODER *encoder, IMAGE *image_array[], int frame_count, BITSTREAM *stream)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
	//CODEC_STATE *codec = &encoder->codec;

	int layer_index;

	// The number of frames must match the number of layers in the sample
	assert(frame_count == encoder->layer_count);

	// Initialize the codec state
	PrepareEncoderState(encoder);

	// Write the bitstream start marker
	error = PutBitstreamStartMarker(stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Write the bitstream header that is common to all layers
	error = EncodeBitstreamHeader(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Write the extension header to the bitstream
	error = EncodeExtensionHeader(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Encode each frame in a separate layer in the sample
	for (layer_index = 0; layer_index < frame_count; layer_index++)
	{
		error = EncodeLayer(encoder, image_array[layer_index]->buffer, image_array[layer_index]->pitch, stream);
		assert(error == CODEC_ERROR_OKAY);
		if (! (error == CODEC_ERROR_OKAY)) {
			return error;
		}
	}

	error = EncodeSampleExtensionTrailer(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Finish the encoded sample after the last layer
	error = EncodeSampleTrailer(encoder, stream);
	assert(error == CODEC_ERROR_OKAY);
	if (! (error == CODEC_ERROR_OKAY)) {
		return error;
	}

	// Force any data remaining in the bitstream to be written into the sample
	FlushBitstream(stream);

	// Check that the sample offset stack has been emptied
	assert(stream->sample_offset_count == 0);

	//TODO: Any resources need to be released?
    
	// Done encoding all layers in the sample
	return error;
}
#endif

/*!
	@brief Initialize the wavelet transforms for encoding
*/
CODEC_ERROR PrepareEncoderTransforms(ENCODER *encoder)
{
	//int channel_count = encoder->channel_count;
	int channel_number;

	// Set the prescale and quantization in each wavelet transform
	for (channel_number = 0; channel_number < encoder->channel_count; channel_number++)
	{
		TRANSFORM *transform = &encoder->transform[channel_number];

		// Set the prescaling (may be used in setting the quantization)
		int bits_per_component = encoder->channel[channel_number].bits_per_component;
		SetTransformPrescale(transform, bits_per_component);

		//TODO: Are the wavelet scale factors still used?

		// Must set the transform scale if not calling SetTransformQuantization
		SetTransformScale(transform);
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Unpack the image into component arrays for encoding
*/
CODEC_ERROR ImageUnpackingProcess(const PACKED_IMAGE *input,
								  UNPACKED_IMAGE *output,
								  const ENCODER_PARAMETERS *parameters,
								  gpr_allocator *allocator)
{
	ENABLED_PARTS enabled_parts = parameters->enabled_parts;
	int channel_count;
	DIMENSION max_channel_width;
	DIMENSION max_channel_height;
	int bits_per_component;

	// The configuration of component arrays is determined by the image format
	switch (input->format)
	{
    case PIXEL_FORMAT_RAW_RGGB_12:
    case PIXEL_FORMAT_RAW_RGGB_12P:
    case PIXEL_FORMAT_RAW_RGGB_14:
    case PIXEL_FORMAT_RAW_GBRG_12:
    case PIXEL_FORMAT_RAW_GBRG_12P:
    case PIXEL_FORMAT_RAW_RGGB_16:
        channel_count = 4;
        max_channel_width = input->width / 2;
        max_channel_height = input->height / 2;
        bits_per_component = 12;
        break;
            
	default:
		assert(0);
		return CODEC_ERROR_PIXEL_FORMAT;
		break;
	}

	// Allocate space for the component arrays
	AllocateComponentArrays(allocator, output, channel_count, max_channel_width, max_channel_height,
		input->format, bits_per_component);

    
    // The configuration of component arrays is determined by the image format
    switch (input->format)
    {
        case PIXEL_FORMAT_RAW_RGGB_14:
            UnpackImage_14(input, output, enabled_parts, true );
            break;

        case PIXEL_FORMAT_RAW_RGGB_12:
            UnpackImage_12(input, output, enabled_parts, true );
            break;

        case PIXEL_FORMAT_RAW_GBRG_12:
            UnpackImage_12(input, output, enabled_parts, false );
            break;

        case PIXEL_FORMAT_RAW_RGGB_12P:
            UnpackImage_12P(input, output, enabled_parts, true );
            break;

        case PIXEL_FORMAT_RAW_GBRG_12P:
            UnpackImage_12P(input, output, enabled_parts, false );
            break;
            
        default:
            assert(0);
            return CODEC_ERROR_PIXEL_FORMAT;
            break;
    }
    
	return CODEC_ERROR_OKAY;
}



/*!
	@brief Insert the header segments that are common to all samples

	This code was derived from PutVideoIntraFrameHeader in the current codec.

	@todo Need to output the channel size table.
*/
CODEC_ERROR EncodeBitstreamHeader(ENCODER *encoder, BITSTREAM *stream)
{
	CODEC_STATE *codec = &encoder->codec;

	//TAGWORD subband_count = 10;
	TAGWORD image_width = encoder->image_width;
	TAGWORD image_height = encoder->image_height;

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	TAGWORD image_format = encoder->image_format;
	TAGWORD pattern_width = encoder->pattern_width;
	TAGWORD pattern_height = encoder->pattern_height;
	TAGWORD components_per_sample = encoder->components_per_sample;
	TAGWORD max_bits_per_component = encoder->max_bits_per_component;
	TAGWORD default_bits_per_component = max_bits_per_component;
#else
	TAGWORD default_bits_per_component = encoder->internal_precision;
#endif

	// Align the start of the header on a segment boundary
	AlignBitsSegment(stream);

	// The bitstream should be aligned to a segment boundary
	assert(IsAlignedSegment(stream));

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_HEADER))
    {
        // Write the section header for the bitstream header into the bitstream
        BeginHeaderSection(encoder, stream);
    }
#endif
    
	// Output the number of channels
	if (encoder->channel_count != codec->channel_count) {
		PutTagPair(stream, CODEC_TAG_ChannelCount, encoder->channel_count);
		codec->channel_count = encoder->channel_count;
	}

	// Inform the decoder of the maximum component array dimensions
	PutTagPair(stream, CODEC_TAG_ImageWidth, image_width);
	PutTagPair(stream, CODEC_TAG_ImageHeight, image_height);

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	if (IsPartEnabled(encoder->enabled_parts, VC5_PART_IMAGE_FORMATS))
	{
		PutTagPair(stream, CODEC_TAG_ImageFormat, image_format);
		PutTagPair(stream, CODEC_TAG_PatternWidth, pattern_width);
		PutTagPair(stream, CODEC_TAG_PatternHeight, pattern_height);
		PutTagPair(stream, CODEC_TAG_ComponentsPerSample, components_per_sample);
		PutTagPair(stream, CODEC_TAG_MaxBitsPerComponent, max_bits_per_component);
	}
#endif

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	if (IsPartEnabled(encoder->enabled_parts, VC5_PART_LAYERS))
	{
		// Output the number of layers in the sample (optional for backward compatibility)
		//PutTagPairOptional(stream, CODEC_TAG_LAYER_COUNT, layer_count);
	}
#endif

	// Record the image dimensions in the codec state
	codec->image_width = image_width;
	codec->image_height = image_height;
	
	// The image dimensions determine the default channel dimensions
	codec->channel_width = image_width;
	codec->channel_height = image_height;

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	if (IsPartEnabled(encoder->enabled_parts, VC5_PART_IMAGE_FORMATS))
	{
		// Record the pattern element parameters in the codec state
		codec->image_format = image_format;
		codec->pattern_width = pattern_width;
		codec->pattern_height = pattern_height;
		codec->components_per_sample = components_per_sample;
		codec->max_bits_per_component = (PRECISION)max_bits_per_component;
	}
#endif

	// This parameter is the default precision for each channel
	codec->bits_per_component = default_bits_per_component;

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_HEADER))
    {
        // Make sure that the bitstream is aligned to a segment boundary
        AlignBitsSegment(stream);
        
        // Update the section header with the actual size of the bitstream header section
        EndSection(stream);
    }
#endif

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the trailer at the end of the encoded sample

	This routine updates the sample size segment in the sample extension header
	with the actual size of the encoded sample.  The size of the encoded sample
	does not include the size of the sample header or trailer.

	Note that the trailer may not be necessary as the decoder may stop
	reading from the sample after it has decoded all of the information
	required to reconstruct the frame.

	This code was derived from PutVideoIntraFrameTrailer in the current codec.
*/
CODEC_ERROR EncodeBitstreamTrailer(ENCODER *encoder, BITSTREAM *stream)
{
	AlignBitsSegment(stream);

	return CODEC_ERROR_OKAY;
}

/*!
 @brief Write the unique image identifier
 
 @todo Should the UMID instance number be a parameter to this routine?
 */
static CODEC_ERROR WriteUniqueImageIdentifier(ENCODER *encoder, BITSTREAM *stream)
{
    const int UMID_length_byte = 0x13;
    const int UMID_instance_number = 0;
    
    // Total length of the unique image identifier chunk payload (in segments)
    const int identifier_chunk_payload_length = UMID_length + sequence_number_length;
    
    // Write the tag value pair for the small chunk element for the unique image identifier
    PutTagPairOptional(stream, CODEC_TAG_UniqueImageIdentifier, identifier_chunk_payload_length);
    
    // Write the UMID label
    PutByteArray(stream, UMID_label, sizeof(UMID_label));
    
    // Write the UMID length byte
    PutBits(stream, UMID_length_byte, 8);
    
    // Write the UMID instance number
    PutBits(stream, UMID_instance_number, 24);
    
    // Write the image sequence identifier
    PutByteArray(stream, encoder->image_sequence_identifier, sizeof(encoder->image_sequence_identifier));
    
    // Write the image sequence number
    PutLong(stream, encoder->image_sequence_number);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write extra information that follows the sample header into the bitstream

	This routine writes metadata into the sample header extension.

	Metadata includes the unique GUID for each video clip, the number of each video frame,
	and the timecode (if available).  The GUID and frame number pair uniquely identify each
	frame in the encoded clip.

	This routine also outputs additional information that describes the characterstics of
    the encoded video in the GOP extension and sample flags.

	The size of the sample extension header is provided by the sample size segment.
*/
CODEC_ERROR EncodeExtensionHeader(ENCODER *encoder, BITSTREAM *stream)
{
    ENABLED_PARTS enabled_parts = encoder->enabled_parts;

    // Encode the transform prescale for the first channel (assume all channels are the same)
	TAGWORD prescale_shift = PackTransformPrescale(&encoder->transform[0]);

	// The tag-value pair is required if the encoder is not using the default values
	//if (IsTransformPrescaleDefault(&encoder->transform[0], TRANSFORM_TYPE_SPATIAL, encoder->encoded.precision))
	if (IsTransformPrescaleDefault(&encoder->transform[0], encoder->internal_precision))
	{
		PutTagPairOptional(stream, CODEC_TAG_PrescaleShift, prescale_shift);
	}
	else
	{
		PutTagPair(stream, CODEC_TAG_PrescaleShift, prescale_shift);
	}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
    {
        WriteUniqueImageIdentifier(encoder, stream);
    }
#endif

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS) &&
        !IsComponentTransformIdentity(encoder->component_transform))
    {
        WriteComponentTransform(encoder->component_transform, stream);
    }
#endif

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS) &&
        !IsComponentPermutationIdentity(encoder->component_permutation))
    {
        WriteComponentPermutation(encoder->component_permutation, stream);
    }
#endif

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the sample extension trailer into the bitstream

	This routine must be called after encoding the sample and before writing the
	sample trailer, but must only be called if the sample extension header was
	written into the bitstream.
*/
CODEC_ERROR EncodeExtensionTrailer(ENCODER *encoder, BITSTREAM *stream)
{
	return CODEC_ERROR_OKAY;
}

static int32_t GetMultiplier(QUANT divisor)
{
    switch (divisor)
    {
        case 1:
            return (uint32_t)(1 << 16);
            
        case 12:
            return (1 << 16) / 12;
            
        case 24:
            return (1 << 16) / 24;
            
        case 32:
            return (1 << 16) / 32;
            
        case 48:
            return (1 << 16) / 48;
            
        case 96:
            return (1 << 16) / 96;
            
        case 144:
            return (1 << 16) / 144;
            
        default:
            return (uint32_t)(1 << 16) / divisor;
    };
}

/*!
 @brief Compute the rounding value for quantization
 */
static QUANT QuantizerMidpoint(QUANT correction, QUANT divisor)
{
    int32_t midpoint = 0;
    
    if (correction == 2)
    {
        midpoint = divisor >> 1;
        
        // CFEncode_Premphasis_Original
        if (midpoint) {
            midpoint--;
        }
    }
    else if (correction > 2 && correction < 9)
    {
        midpoint = divisor / correction;
    }
    
    return midpoint;
}

static void GetQuantizationParameters(int32_t midpoint_prequant, QUANT quant[], int32_t* midpoints, int32_t* multipliers)
{
    int i;
    for (i = 0; i < 4; i++)
    {
        midpoints[i] = QuantizerMidpoint(midpoint_prequant, quant[i]);
        multipliers[i] = GetMultiplier(quant[i]);
    }
}

/*!
	@brief Shift the buffers of horizontal highpass results
 
	The encoder contains six rows of horizontal lowpass and highpass results
	for each channel.  This routine shifts the buffers by two rows to make
	room for two more rows of horizontal results for each channel.
 */
static void ShiftHorizontalResultBuffers(PIXEL **buffer)
{
    PIXEL *buffer01[2];
    
    memcpy( buffer01, buffer + 0, sizeof(PIXEL*) * 2 );
    
    memmove( buffer + 0, buffer + 2, sizeof(PIXEL*) * (ROW_BUFFER_COUNT - 2) );

    memcpy( buffer + 4, buffer01, sizeof(PIXEL*) * 2 );
}

typedef struct _recursive_transform_data
{
    PIXEL       *input_ptr;
    DIMENSION   input_width;
    DIMENSION   input_height;
    DIMENSION   input_pitch;

    PIXEL       *output_ptr[MAX_BAND_COUNT];
    DIMENSION   output_width;
    DIMENSION   output_pitch;
    
    int32_t     prescale;
    
    int32_t*    midpoints;
    int32_t*    multipliers;
    
    PIXEL       **lowpass_buffer;
    PIXEL       **highpass_buffer;
    
} RECURSIVE_TRANSFORM_DATA;

#define RECURSIVE 1

static void ForwardWaveletTransformRecursive(RECURSIVE_TRANSFORM_DATA *transform_data, int wavelet_stage, uint32_t start_row, uint32_t end_row)
{
	uint32_t input_row_index = start_row;
 
    PIXEL       *input_ptr          = transform_data[wavelet_stage].input_ptr;
    DIMENSION   input_width         = transform_data[wavelet_stage].input_width;
    DIMENSION   input_height        = transform_data[wavelet_stage].input_height;
    DIMENSION   input_pitch         = transform_data[wavelet_stage].input_pitch;
    
    PIXEL       **output_ptr        = transform_data[wavelet_stage].output_ptr;
    DIMENSION   output_width        = transform_data[wavelet_stage].output_width;
    DIMENSION   output_pitch        = transform_data[wavelet_stage].output_pitch;
    
    int32_t*     midpoints          = transform_data[wavelet_stage].midpoints;
    int32_t*     multipliers        = transform_data[wavelet_stage].multipliers;
    
    PIXEL       **lowpass_buffer    = transform_data[wavelet_stage].lowpass_buffer;
    PIXEL       **highpass_buffer   = transform_data[wavelet_stage].highpass_buffer;
    
    int32_t     prescale            = transform_data[wavelet_stage].prescale;
    
    int bottom_input_row = ((input_height % 2) == 0) ? input_height - 2 : input_height - 1;
    
	uint32_t last_middle_row = bottom_input_row - 2;
    
    end_row = minimum( last_middle_row, end_row);
    
    // --- TOP ROW
    if( input_row_index == 0 )
    {
        int row;
        
        for (row = 0; row < ROW_BUFFER_COUNT; row++)
        {
            PIXEL *input_row_ptr = (PIXEL *)((uintptr_t)input_ptr + row * input_pitch);
            
            FilterHorizontalRow(input_row_ptr, lowpass_buffer[row], highpass_buffer[row], input_width, prescale);
        }
        
        // Process the first row as a special case for the boundary condition
        FilterVerticalTopRow(lowpass_buffer, highpass_buffer, output_ptr, output_width, output_pitch, midpoints, multipliers, input_row_index );
        input_row_index += 2;
    }
    
    // --- MIDDLE ROWS
    for (; input_row_index <= end_row; input_row_index += 2)
    {
        // Check for errors in the row calculation
        assert((input_row_index % 2) == 0);
        
        FilterVerticalMiddleRow(lowpass_buffer, highpass_buffer, output_ptr, output_width, output_pitch, midpoints, multipliers, input_row_index );

        if (input_row_index < last_middle_row)
        {
            int row;
            
            ShiftHorizontalResultBuffers(lowpass_buffer);
            ShiftHorizontalResultBuffers(highpass_buffer);
            
            // Get two more rows of horizontal lowpass and highpass results
            for (row = 4; row < ROW_BUFFER_COUNT; row++)
            {
                int next_input_row = minimum( input_row_index + row, input_height - 1 );
                
                PIXEL *input_row_ptr = (PIXEL *)((uintptr_t)input_ptr + next_input_row * input_pitch);
                
                FilterHorizontalRow(input_row_ptr, lowpass_buffer[row], highpass_buffer[row], input_width, prescale);
            }
        }
    }
    
    // --- BOTTOM ROW
    if( input_row_index == bottom_input_row )
    {
        FilterVerticalBottomRow(lowpass_buffer, highpass_buffer, output_ptr, output_width, output_pitch, midpoints, multipliers, input_row_index );
    }
    
    if( wavelet_stage < (MAX_WAVELET_COUNT - 1) )
    {
        ForwardWaveletTransformRecursive( transform_data, wavelet_stage + 1, 0, 0xFFFF );
    }
}

static void SetRecursiveTransformData(RECURSIVE_TRANSFORM_DATA* transform_data,
                                      const TRANSFORM *transform,
                                      const COMPONENT_ARRAY *input_image_component,
                                      int32_t midpoints[MAX_BAND_COUNT], int32_t multipliers[MAX_BAND_COUNT],
                                      PIXEL *lowpass_buffer[MAX_WAVELET_COUNT][ROW_BUFFER_COUNT],
                                      PIXEL *highpass_buffer[MAX_WAVELET_COUNT][ROW_BUFFER_COUNT],
                                      int midpoint_prequant, int wavelet_stage )
{
    int i;
    
    if( wavelet_stage == 0 )
    {
        transform_data->input_width  = input_image_component->width;
        transform_data->input_height = input_image_component->height;
        transform_data->input_pitch  = input_image_component->pitch;
        transform_data->input_ptr    = (PIXEL*)input_image_component->data;
    }
    else
    {
        WAVELET *input_wavelet = transform->wavelet[wavelet_stage - 1];
        
        transform_data->input_width  = input_wavelet->width;
        transform_data->input_height = input_wavelet->height;
        transform_data->input_pitch  = input_wavelet->pitch;
        transform_data->input_ptr    = WaveletRowAddress(input_wavelet, LL_BAND, 0);
    }
    
    WAVELET *output_wavelet = transform->wavelet[wavelet_stage];
    assert(output_wavelet);
    
    transform_data->output_width    = output_wavelet->width;
    transform_data->output_pitch    = output_wavelet->pitch;

    for (i = 0; i < MAX_BAND_COUNT; i++)
    {
        transform_data->output_ptr[i] = output_wavelet->data[i];
    }
    
    transform_data->lowpass_buffer  = lowpass_buffer[wavelet_stage];
    transform_data->highpass_buffer = highpass_buffer[wavelet_stage];
    transform_data->prescale        = transform->prescale[wavelet_stage];
    
    GetQuantizationParameters(midpoint_prequant, output_wavelet->quant, midpoints, multipliers );
    
    transform_data->midpoints       = midpoints;
    transform_data->multipliers     = multipliers;
}

static void ForwardWaveletTransform(TRANSFORM *transform, const COMPONENT_ARRAY *input_image_component, PIXEL *lowpass_buffer[MAX_WAVELET_COUNT][ROW_BUFFER_COUNT], PIXEL *highpass_buffer[MAX_WAVELET_COUNT][ROW_BUFFER_COUNT], int midpoint_prequant)
{
    RECURSIVE_TRANSFORM_DATA transform_data[MAX_WAVELET_COUNT];
    
    int32_t midpoints[MAX_WAVELET_COUNT][MAX_BAND_COUNT];      //!< Midpoint value for each band (applied during quantization)
    int32_t multipliers[MAX_WAVELET_COUNT][MAX_BAND_COUNT];	//!< Multiplier value for each band (applied during quantization)
    
    SetRecursiveTransformData( &transform_data[0], transform, input_image_component, midpoints[0], multipliers[0], lowpass_buffer, highpass_buffer, midpoint_prequant, 0 );
    SetRecursiveTransformData( &transform_data[1], transform, input_image_component, midpoints[1], multipliers[1], lowpass_buffer, highpass_buffer, midpoint_prequant, 1 );
    SetRecursiveTransformData( &transform_data[2], transform, input_image_component, midpoints[2], multipliers[2], lowpass_buffer, highpass_buffer, midpoint_prequant, 2 );
    
    ForwardWaveletTransformRecursive( transform_data, 0, 0, 0xFFFF );
}

/*!
	@brief Encode the portion of a sample that corresponds to a single layer

	Samples can be contain multiple subsamples.  Each subsample may correspond to
	a different view.  For example, an encoded video sample may contain both the
	left and right subsamples in a stereo pair.

	Subsamples have been called tracks or channels, but this terminology can be
	confused with separate video tracks in a multimedia container or the color
	planes that are called channels elsewhere in this codec.

	The subsamples are decoded seperately and composited to form a single frame
	that is the output of the complete process of decoding a single video sample.
	For this reason, the subsamples are called layers.

	@todo Need to reset the codec state for each layer?
*/
//CODEC_ERROR EncodeLayer(ENCODER *encoder, void *buffer, size_t pitch, BITSTREAM *stream)
CODEC_ERROR EncodeMultipleChannels(ENCODER *encoder, const UNPACKED_IMAGE *image, BITSTREAM *stream)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
    
	int channel_count;
	int channel_index;

	channel_count = encoder->channel_count;
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	if (IsPartEnabled(encoder->enabled_parts, VC5_PART_LAYERS))
	{
		// Write the tag value pairs that preceed the encoded wavelet tree
		error = EncodeLayerHeader(encoder, stream);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}
	}
#endif

    
    CODEC_STATE *codec = &encoder->codec;
    
	// Compute the wavelet transform tree for each channel
	for (channel_index = 0; channel_index < channel_count; channel_index++)
	{
        int channel_number;
        
        ForwardWaveletTransform(&encoder->transform[channel_index], &image->component_array_list[channel_index], encoder->lowpass_buffer, encoder->highpass_buffer, encoder->midpoint_prequant );

        channel_number = encoder->channel_order_table[channel_index];
        
        // Encode the tag value pairs in the header for this channel
        error = EncodeChannelHeader(encoder, channel_number, stream);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Encode the lowpass and highpass bands in the wavelet tree for this channel
        error = EncodeChannelSubbands(encoder, channel_number, stream);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Encode the tag value pairs in the trailer for this channel
        error = EncodeChannelTrailer(encoder, channel_number, stream);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Check that the bitstream is alligned to a segment boundary
        assert(IsAlignedSegment(stream));
        
        // Update the codec state for the next channel in the bitstream
        //codec->channel_number++;
        codec->channel_number = (channel_number + 1);
        codec->subband_number = 0;
	}
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	if (IsPartEnabled(encoder->enabled_parts, VC5_PART_LAYERS))
	{
		// Write the tag value pairs that follow the encoded wavelet tree
		error = EncodeLayerTrailer(encoder, stream);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}
		//TODO: Need to align the bitstream between layers?
	}
#endif
    
	return CODEC_ERROR_OKAY;
}

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Write the sample layer header

	The baseline profile only supports a single layer so the layer header
	and trailer are not required.
*/
CODEC_ERROR EncodeLayerHeader(ENCODER *encoder, BITSTREAM *stream)
{
	//TODO: Write the tag-value pair for the layer number

	return CODEC_ERROR_OKAY;
}
#endif
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Write the sample layer trailer

	The baseline profile only supports a single layer so the layer header
	and trailer are not required.

	If more than one layer is present, the layers must be terminated by a
	layer trailer.  Otherwise, the decoder will continue to parse tag-value
	pairs that belong to the next layer.
*/
CODEC_ERROR EncodeLayerTrailer(ENCODER *encoder, BITSTREAM *stream)
{
	// The value in the layer trailer tag-value pair is not used
	PutTagPairOptional(stream, CODEC_TAG_LAYER_TRAILER, 0);

	return CODEC_ERROR_OKAY;
}
#endif

/*!
	@brief Encode the channel into the bistream

	This routine encodes all of the subbands (lowpass and highpass) in the
	wavelet tree for the specified channel into the bitstream.
*/
CODEC_ERROR EncodeChannelWavelets(ENCODER *encoder, BITSTREAM *stream)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
	CODEC_STATE *codec = &encoder->codec;

	int channel_count;
	int channel_index;

	// Get the number of channels in the encoder wavelet transform
	channel_count = encoder->channel_count;

	// Compute the remaining wavelet transforms for each channel
	//for (channel_index = 0; channel_index < channel_count; channel_index++)
	for (channel_index = 0; channel_index < channel_count; channel_index++)
	{
		int channel_number = encoder->channel_order_table[channel_index];

        // Encode the tag value pairs in the header for this channel
		error = EncodeChannelHeader(encoder, channel_number, stream);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}

		// Encode the lowpass and highpass bands in the wavelet tree for this channel
		error = EncodeChannelSubbands(encoder, channel_number, stream);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}

		// Encode the tag value pairs in the trailer for this channel
		error = EncodeChannelTrailer(encoder, channel_number, stream);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}

        // Check that the bitstream is alligned to a segment boundary
		assert(IsAlignedSegment(stream));

		// Update the codec state for the next channel in the bitstream
		//codec->channel_number++;
		codec->channel_number = (channel_number + 1);
		codec->subband_number = 0;
	}
    
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the channel header into the bitstream
	
	The channel header separates channels in the encoded layer.  The channel header
	is not required before the first encoded channel because the codec state is
	initialized for decoding the first channel.
	
	The first channel is channel number zero.
*/
CODEC_ERROR EncodeChannelHeader(ENCODER *encoder,
								int channel_number,
								BITSTREAM *stream)
{
	CODEC_STATE *codec = &encoder->codec;
	DIMENSION channel_width = encoder->channel[channel_number].width;
	DIMENSION channel_height = encoder->channel[channel_number].height;
	int bits_per_component = encoder->channel[channel_number].bits_per_component;

	AlignBitsSegment(stream);

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_CHANNEL))
    {
        // Write the channel section header into the bitstream
        BeginChannelSection(encoder, stream);
    }
#endif
    
	// Write the channel number if it does not match the codec state
	if (channel_number != codec->channel_number)
	{
		PutTagPair(stream, CODEC_TAG_ChannelNumber, channel_number);
		codec->channel_number = channel_number;
	}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	if (IsPartEnabled(encoder->enabled_parts, VC5_PART_IMAGE_FORMATS))
	{
		// The decoder will derive the channel width and height from the image dimensions and format
		codec->channel_width = channel_width;
		codec->channel_height = channel_height;
	}
	else
#endif
	{
		// Write the component array width if it does not match the codec state
		if (channel_width != codec->channel_width)
		{
			PutTagPair(stream, CODEC_TAG_ChannelWidth, channel_width);
			codec->channel_width = channel_width;
		}

		// Write the component array height if it does not match the codec state
		if (channel_height != codec->channel_height)
		{
			PutTagPair(stream, CODEC_TAG_ChannelHeight, channel_height);
			codec->channel_height = channel_height;
		}
	}

	// Write the component array precision if it does not match the codec state
	if (bits_per_component != codec->bits_per_component)
	{
		PutTagPair(stream, CODEC_TAG_BitsPerComponent, bits_per_component);
		codec->bits_per_component = bits_per_component;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the encoded subbands for this channel into the bitstream

	This routine writes the encoded subbands in the wavelet tree for this channel
	into the bitstream, including both the lowpass band and all of the highpass
	bands in each wavelet in this channel.
*/
CODEC_ERROR EncodeChannelSubbands(ENCODER *encoder, int channel_number, BITSTREAM *stream)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
	//CODEC_STATE *codec = &encoder->codec;

	int wavelet_count = encoder->wavelet_count;
	int last_wavelet_index = wavelet_count - 1;
	int wavelet_index;

	int subband = 0;

	// Start with the lowpass band in the wavelet at the highest level
	WAVELET *wavelet = encoder->transform[channel_number].wavelet[last_wavelet_index];

	// Check that the bitstream is aligned on a segment boundary
	assert(IsAlignedSegment(stream));

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_WAVELET))
    {
        // Write the wavelet section header into the bitstream
        BeginWaveletSection(encoder, stream);
    }
#endif
    
	// Encode the lowpass subband in this channel
	error = EncodeLowpassBand(encoder, wavelet, channel_number, stream);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}

	// Advance to the first highpass subband
	subband++;

	// Encode the highpass bands in order of subband number
	for (wavelet_index = last_wavelet_index; wavelet_index >= 0; wavelet_index--)
	{
		//int wavelet_type = WAVELET_TYPE_SPATIAL;
		//int wavelet_level = wavelet_index + 1;
		int band_index;

		//int lowpass_scale = 0;
		//int lowpass_divisor = 0;

		wavelet = encoder->transform[channel_number].wavelet[wavelet_index];

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
        if (IsSectionEnabled(encoder, SECTION_NUMBER_WAVELET))
        {
            // Was the wavelet section header already written into the bitstream?
            if (wavelet_index < last_wavelet_index)
            {
                // Write the wavelet section header into the bitstream
                BeginWaveletSection(encoder, stream);
            }
        }
#endif
		// Encode the highpass bands in this wavelet
		for (band_index = 1; band_index < wavelet->band_count; band_index++)
		{
			error = EncodeHighpassBand(encoder, wavelet, band_index, subband, stream);
			if (error != CODEC_ERROR_OKAY) {
				return error;
			}
            
			// Advance to the next subband
			subband++;
		}
        
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
        if (IsSectionEnabled(encoder, SECTION_NUMBER_WAVELET))
        {
            // Make sure that the bitstream is aligned to a segment boundary
            AlignBitsSegment(stream);
            
            // Update the section header with the actual size of the wavelet section
            EndSection(stream);
        }
#endif
	}
    
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the channel trailer into the bitstream

	A channel trailer is not required as the channel header functions as a marker
	between channels in the bitstream.
	
	It may be necessary to update the channel size in a sample size segment written
	into the channel header if the channel header includes a sample size segment in
	the future.
*/
CODEC_ERROR EncodeChannelTrailer(ENCODER *encoder, int channel, BITSTREAM *stream)
{
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_CHANNEL))
    {
        // Make sure that the bitstream is aligned to a segment boundary
        AlignBitsSegment(stream);
        
        // Update the section header with the actual size of the channel section
        EndSection(stream);
    }
#endif
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Allocate intermediate buffers for the horizontal transform results

	@todo Need to return an error code if allocation fails
*/
CODEC_ERROR AllocateEncoderHorizontalBuffers(ENCODER *encoder)
{
	gpr_allocator *allocator = encoder->allocator;
    int channel_index;
	int wavelet_index;
	int channel_count = encoder->channel_count;
    
    int buffer_width = 0;
    
    for (channel_index = 0; channel_index < channel_count; channel_index++)
    {
        buffer_width = maximum(buffer_width, encoder->channel[channel_index].width );
    }
    
    buffer_width = ((buffer_width % 2) == 0) ? buffer_width / 2 : (buffer_width + 1) / 2;
    
    for (wavelet_index = 0; wavelet_index < MAX_WAVELET_COUNT; wavelet_index++)
    {
        int row;
        
        int channel_width = encoder->transform[0].wavelet[wavelet_index]->width;
        
        for (row = 0; row < ROW_BUFFER_COUNT; row++)
        {
            PIXEL *lowpass_buffer  = allocator->Alloc(channel_width * sizeof(PIXEL) * 2);
            PIXEL *highpass_buffer = lowpass_buffer + channel_width;

            assert(lowpass_buffer != NULL);
            if (! (lowpass_buffer != NULL)) {
                return CODEC_ERROR_OUTOFMEMORY;
            }
            
            encoder->lowpass_buffer[wavelet_index][row]  = lowpass_buffer;
            encoder->highpass_buffer[wavelet_index][row] = highpass_buffer;
        }
    }
    

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Deallocate the intermediate buffers for the horizontal transform results

	It is possible to avoid reallocating the buffers for the horizontal transform
	results if the buffers were not deallocated between encoded frames.  In this case,
	it would be necessary to call this routine inside @ref ReleaseEncoder and it would
	also be necessary to modify @ref AllocateEncoderHorizontalBuffers to not allocate
	the buffers if they are already allocated.
*/
CODEC_ERROR DeallocateEncoderHorizontalBuffers(ENCODER *encoder)
{
	gpr_allocator *allocator = encoder->allocator;

	int wavelet_index;

    for (wavelet_index = 0; wavelet_index < MAX_WAVELET_COUNT; wavelet_index++)
    {
        int row;
        
        for (row = 0; row < ROW_BUFFER_COUNT; row++)
        {
            allocator->Free(encoder->lowpass_buffer[wavelet_index][row]);
        }
    }
    
    
	return CODEC_ERROR_OKAY;
}

/*!
	@brief Allocate buffers used for computing the forward wavelet transform
*/
CODEC_ERROR AllocateHorizontalBuffers(gpr_allocator *allocator,
									  PIXEL *lowpass_buffer[],
									  PIXEL *highpass_buffer[],
									  int buffer_width)
{
	const size_t row_buffer_size = buffer_width * sizeof(PIXEL);

	int row;

	for (row = 0; row < ROW_BUFFER_COUNT; row++)
	{
		lowpass_buffer[row] = allocator->Alloc(row_buffer_size);
		highpass_buffer[row] = allocator->Alloc(row_buffer_size);

		// Check that the memory allocation was successful
		assert(lowpass_buffer[row] != NULL);
		if (! (lowpass_buffer[row] != NULL)) {
			return CODEC_ERROR_OUTOFMEMORY;
		}
		assert(highpass_buffer[row] != NULL);
		if (! (highpass_buffer[row] != NULL)) {
			return CODEC_ERROR_OUTOFMEMORY;
		}
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Deallocate buffers used for computing the forward wavelet transform
*/
CODEC_ERROR DeallocateHorizontalBuffers(gpr_allocator *allocator,
										PIXEL *lowpass_buffer[],
										PIXEL *highpass_buffer[])
{
	int row;

	for (row = 0; row < ROW_BUFFER_COUNT; row++)
	{
		allocator->Free(lowpass_buffer[row]);
		allocator->Free(highpass_buffer[row]);
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Allocate all of the wavelets used during encoding

	This routine allocates all of the wavelets in the wavelet tree that
	may be used during encoding.

	This routine is used to preallocate the wavelets before encoding begins.
	If the wavelet bands are allocated on demand if not preallocated.

	By default, the wavelet bands are encoded into the bitstream with the bands
	from the wavelet at the highest level (smallest wavelet) first so that the
	bands can be processed by the encoder in the order as the sample is decoded.

	@todo Do not allocate wavelets for resolutions that are larger then the
	decoded resolution.  At lower resolutions, the depth of the wavelet tree
	can be reduced and the highpass bands in the unused wavelets to not have
	to be decoded.

	@todo Should it be an error if the wavelets are not preallocated?
*/
CODEC_ERROR AllocEncoderTransforms(ENCODER *encoder)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;

	// Use the default allocator for the encoder
	gpr_allocator *allocator = encoder->allocator;
	int channel_index;
	int wavelet_index;

	assert(encoder != NULL);
	if (! (encoder != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	// Check that the encoded dimensions are valid
	//assert((encoder->encoded.width % (1 << encoder->wavelet_count)) == 0);

	for (channel_index = 0; channel_index < encoder->channel_count; channel_index++)
	{
		// The wavelet at level zero has the same dimensions as the encoded frame
		DIMENSION wavelet_width = 0;
		DIMENSION wavelet_height = 0;
		error = GetChannelDimensions(encoder, channel_index, &wavelet_width, &wavelet_height);
		assert(wavelet_width > 0 && wavelet_height > 0);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}

		for (wavelet_index = 0; wavelet_index < encoder->wavelet_count; wavelet_index++)
		{
			WAVELET *wavelet = NULL;

			// Pad the wavelet width if not divisible by two
			if ((wavelet_width % 2) != 0) {
				wavelet_width++;
			}

			// Pad the wavelet height if not divisible by two
			if ((wavelet_height % 2) != 0) {
				wavelet_height++;
			}

			// Reduce the dimensions of the next wavelet by half
			wavelet_width /= 2;
			wavelet_height /= 2;

			// Dimensions of the current wavelet must be divisible by two
			//assert((wavelet_width % 2) == 0 && (wavelet_height % 2) == 0);

			// The wavelet width must be divisible by two
			//assert((wavelet_width % 2) == 0);

			// Allocate the wavelet
			wavelet = CreateWavelet(allocator, wavelet_width, wavelet_height);
			if (wavelet == NULL) {
				return CODEC_ERROR_OUTOFMEMORY;
			}

			// Add the wavelet to the transform
			encoder->transform[channel_index].wavelet[wavelet_index] = wavelet;
		}
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Allocate all of the buffers required for encoding

	This routine allocates buffers required for encoding, not including
	the wavelet images in the wavelet tree which are allocated by
	@ref AllocEncoderTransforms

	This routine is used to preallocate buffers before encoding begins.
	If the buffers are allocated on demand if not preallocated.

	The encoding parameters, including the encoded frame dimensions,
	resolution of the decoded frame, and the decoded pixel format, are
	taken into account when the buffers are allocated.  For example,
	buffer space that is only used when encoding to full resolution will
	not be allocated if the frame is decoded to a smaller size.

	Note that it is not an error to preallocate more buffer space than
	what is strictly required for encoding.  For example, it is okay to
	allocate buffer space required for full frame encoding even if the
	encoded sample will be decoded at lower resolution.  In many applications,
	it is simpler to preallocate the maximum buffer space that may be needed.

	Currently, the reference encoder allocates scratch buffers as required
	by each routine that needs scratch space and the scratch buffers are
	deallocated at the end each routine that allocates scratch space.
	A custom memory allocator can make this scheme efficient.  See comments
	in the documentation for the memory allocator module.

	@todo Should it be an error if the buffers are not preallocated?
*/
CODEC_ERROR AllocEncoderBuffers(ENCODER *encoder)
{
	(void)encoder;
	return CODEC_ERROR_UNIMPLEMENTED;
}

/*!
	@brief Set the quantization parameters in the encoder

	This routine computes the parameters in the quantizer used by
	the encoder based based on the quality setting and the desired
	bitrate.  The quantization parameters are adjsuted to compensate
	for the precision of the input pixels.
	
	Note that the baseline profile does not support quantization to
	achieve a desired bitrate.

*/
CODEC_ERROR SetEncoderQuantization(ENCODER *encoder,
								   const ENCODER_PARAMETERS *parameters)
{
	int channel_count = encoder->channel_count;
	int channel_number;

	const int quant_table_length = sizeof(parameters->quant_table)/sizeof(parameters->quant_table[0]);

    // Set the midpoint prequant parameter
    encoder->midpoint_prequant = 2;
    
	// Set the quantization table in each channel
	for (channel_number = 0; channel_number < channel_count; channel_number++)
	{
		SetTransformQuantTable(encoder, channel_number, parameters->quant_table, quant_table_length);
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Copy the quantization table into the wavelet bands
*/
CODEC_ERROR SetTransformQuantTable(ENCODER *encoder, int channel, const QUANT table[], int table_length)
{
	int wavelet_count = encoder->wavelet_count;
	int wavelet_index;
	int subband;

	// All lowpass bands use the quantization for subband zero
	for (wavelet_index = 0; wavelet_index < wavelet_count; wavelet_index++)
	{
		WAVELET *wavelet = encoder->transform[channel].wavelet[wavelet_index];
		wavelet->quant[0] = table[0];
	}

	// Store the quantization values for the highpass bands in each wavelet
	for (subband = 1; subband < table_length; subband++)
	{
		int wavelet_index = SubbandWaveletIndex(subband);
		int band_index = SubbandBandIndex(subband);
		WAVELET *wavelet;

		assert(0 <= wavelet_index && wavelet_index < wavelet_count);
		assert(0 <= band_index && band_index <= MAX_BAND_COUNT);

		// Store the quantization value for this subband
		wavelet = encoder->transform[channel].wavelet[wavelet_index];
		wavelet->quant[band_index] = table[subband];
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Return the encoded dimensions for the specified channel

	The encoded dimensions for each channel may differ due to color
	difference component sampling.
*/
CODEC_ERROR GetChannelDimensions(ENCODER *encoder,
								 int channel_number,
								 DIMENSION *channel_width_out,
								 DIMENSION *channel_height_out)
{
	DIMENSION channel_width = 0;
	DIMENSION channel_height = 0;

	assert(encoder != NULL && channel_width_out != NULL && channel_height_out != NULL);
	if (! (encoder != NULL && channel_width_out != NULL && channel_height_out != NULL)) {
		return CODEC_ERROR_NULLPTR;
	}

	assert(0 <= channel_number && channel_number < encoder->channel_count);
	if (! (0 <= channel_number && channel_number < encoder->channel_count)) {
		return CODEC_ERROR_UNEXPECTED;
	}

	// Clear the output dimensions in case this routine terminates early
	*channel_width_out = 0;
	*channel_height_out = 0;

	channel_width = encoder->channel[channel_number].width;
	channel_height = encoder->channel[channel_number].height;

	*channel_width_out = channel_width;
	*channel_height_out = channel_height;

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Adjust the height of encoded layer

	Interleaved frames are encoded as separate layers with half the height.
*/
DIMENSION EncodedLayerHeight(ENCODER *encoder, DIMENSION height)
{
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	assert(encoder != NULL);
	if (encoder->progressive == 0) {
		height /= 2;
	}
#endif

	return height;
}

/*!
	@brief Compute the dimensions of the image as reported by the ImageWidth and ImageHeight parameters

	The image width is the maximum width of all component arrays and the image height is the maximum height
	of all component arrays.
*/
CODEC_ERROR GetMaximumChannelDimensions(const UNPACKED_IMAGE *image, DIMENSION *width_out, DIMENSION *height_out)
{
	DIMENSION width = 0;
	DIMENSION height = 0;
	int channel_number;

	if (image == NULL) {
		return CODEC_ERROR_UNEXPECTED;
	}

	for (channel_number = 0; channel_number < image->component_count; channel_number++)
	{
		if (width < image->component_array_list[channel_number].width) {
			width = image->component_array_list[channel_number].width;
		}

		if (height < image->component_array_list[channel_number].height) {
			height = image->component_array_list[channel_number].height;
		}
	}

	if (width_out != NULL) {
		*width_out = width;
	}

	if (height_out != NULL) {
		*height_out = height;
	}

	return CODEC_ERROR_OKAY;
}

/*!
	@brief Set the bit for the specified subband in the decoded band mask

	The decoded subband mask is used to track which subbands have been
	decoded in teh current channel.  It is reset at the start of each
	channel.

	The decoded subband mask is used when decoding a sample at less
	than full resolution.  The mask indicates when enough subbands
	have been decoded for a channel and that remaining portion of the
	encoded sample for the current channel may be skipped.
*/
CODEC_ERROR SetEncodedBandMask(CODEC_STATE *codec, int subband)
{
	if (0 <= subband && subband < MAX_SUBBAND_COUNT) {
		codec->decoded_subband_mask |= (1 << subband);
	}
	return CODEC_ERROR_OKAY;
}


/*!
	@brief Encoded the lowpass band from the bitstream

	The wavelet at the highest level is passes as an argument.
	This routine decodes lowpass band in the bitstream into the
	lowpass band of the wavelet.
*/
CODEC_ERROR EncodeLowpassBand(ENCODER *encoder, WAVELET *wavelet, int channel_number, BITSTREAM *stream)
{
	CODEC_STATE *codec = &encoder->codec;
	//FILE *logfile = encoder->logfile;
	//int subband = 0;
	//int level = encoder->wavelet_count;
	int width = wavelet->width;
	int height = wavelet->height;
	uint8_t *lowpass_row_ptr;
	int lowpass_pitch;
	int row;

	PRECISION lowpass_precision = encoder->channel[channel_number].lowpass_precision;

	lowpass_row_ptr = (uint8_t *)wavelet->data[LL_BAND];
	lowpass_pitch = wavelet->pitch;

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_SUBBAND))
    {
        // Make sure that the bitstream is aligned to a segment boundary
        AlignBitsSegment(stream);
        
        // Write the channel section header into the bitstream
        BeginSubbandSection(encoder, stream);
    }
#endif
    
	// Write the tag-value pairs for the lowpass band to the bitstream
	PutVideoLowpassHeader(encoder, channel_number, stream);

	// Check that the bitstream is tag aligned before writing the pixels
	assert(IsAlignedSegment(stream));

	for (row = 0; row < height; row++)
	{
		uint16_t *lowpass = (uint16_t *)lowpass_row_ptr;
		int column;

		for (column = 0; column < width; column++)
		{
			BITWORD coefficient = lowpass[column];
			//assert(0 <= lowpass[column] && lowpass[column] <= COEFFICIENT_MAX);
			assert(lowpass[column] <= COEFFICIENT_MAX);
			assert(coefficient <= COEFFICIENT_MAX);
			PutBits(stream, coefficient, lowpass_precision);
		}

		lowpass_row_ptr += lowpass_pitch;
	}

	// Align the bitstream to a segment boundary
	AlignBitsSegment(stream);

	PutVideoLowpassTrailer(stream);

	// Update the subband number in the codec state
	codec->subband_number++;

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_SUBBAND))
    {
        // Make sure that the bitstream is aligned to a segment boundary
        AlignBitsSegment(stream);
        
        // Update the section header with the actual size of the subband section
        EndSection(stream);
    }
#endif
    
	return CODEC_ERROR_OKAY;
}

CODEC_ERROR PutVideoSubbandHeader(ENCODER *encoder, int subband_number, QUANT quantization, BITSTREAM *stream)
{
    CODEC_STATE *codec = &encoder->codec;
    
    if (subband_number != codec->subband_number) {
        PutTagPair(stream, CODEC_TAG_SubbandNumber, subband_number);
        codec->subband_number = subband_number;
    }
    
    if (quantization != codec->band.quantization) {
        PutTagPair(stream, CODEC_TAG_Quantization, quantization);
        codec->band.quantization = quantization;
    }
    
    // Write the chunk header for the codeblock
    PushSampleSize(stream, CODEC_TAG_LargeCodeblock);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Encode the highpass band into the bitstream

	The specified wavelet band is decoded from the bitstream
	using the codebook and encoding method specified in the
	bitstream.
*/
CODEC_ERROR EncodeHighpassBand(ENCODER *encoder, WAVELET *wavelet, int band, int subband, BITSTREAM *stream)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;
	CODEC_STATE *codec = &encoder->codec;

	DIMENSION band_width = wavelet->width;
	DIMENSION band_height = wavelet->height;

	void *band_data = wavelet->data[band];
	DIMENSION band_pitch = wavelet->pitch;

	QUANT quantization = wavelet->quant[band];
	//uint16_t scale = wavelet->scale[band];

	//int divisor = 0;
	//int peaks_coding = 0;

	ENCODER_CODESET *codeset = encoder->codeset;

	//int encoding_method = BAND_ENCODING_RUNLENGTHS;

	// Check that the band header starts on a tag boundary
	assert(IsAlignedTag(stream));

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_SUBBAND))
    {
        // Make sure that the bitstream is aligned to a segment boundary
        AlignBitsSegment(stream);
        
        // Write the channel section header into the bitstream
        BeginSubbandSection(encoder, stream);
    }
#endif
    
	// Output the tag-value pairs for this subband
	PutVideoSubbandHeader(encoder, subband, quantization, stream);

	// Encode the highpass coefficients for this subband into the bitstream
	error = EncodeHighpassBandRowRuns(stream, codeset, band_data, band_width, band_height, band_pitch);
	if (error != CODEC_ERROR_OKAY) {
		return error;
	}
    
	// Align the bitstream to a segment boundary
	AlignBitsSegment(stream);

	// Output the band trailer
	PutVideoSubbandTrailer(encoder, stream);

	// Update the subband number in the codec state
	codec->subband_number++;

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsSectionEnabled(encoder, SECTION_NUMBER_SUBBAND))
    {
        // Make sure that the bitstream is aligned to a segment boundary
        AlignBitsSegment(stream);
        
        // Update the section header with the actual size of the subband section
        EndSection(stream);
    }
#endif
    
	return CODEC_ERROR_OKAY;
}

STATIC_INLINE void write_bits(uint8_t** buffer, uint32_t bits)
{
    uint32_t word = Swap32(bits);
    *( (uint32_t*)(*buffer) ) = word;
}

STATIC_INLINE VLE PutZeroBits(uint8_t** buffer, VLE stream_bits, uint_fast8_t size )
{
    BITCOUNT unused_bit_count = bit_word_count - stream_bits.size;
    
    if ( size > unused_bit_count )
    {
        if (stream_bits.size < bit_word_count)
        {
            size -= unused_bit_count;
        }

        write_bits(buffer, stream_bits.bits);
        *buffer += 4;
        
        stream_bits.size = size;
        stream_bits.bits = 0;
    }
    else
    {
        stream_bits.size += size;
    }
    
    return stream_bits;
}

STATIC_INLINE VLE PutBitsCore(uint8_t** buffer, VLE stream_bits, uint32_t bits, uint_fast8_t size )
{
    BITCOUNT unused_bit_count = bit_word_count - stream_bits.size;
    
    if ( size > unused_bit_count)
    {
        if (stream_bits.size < bit_word_count)
        {
            stream_bits.bits |= (bits >> (size - unused_bit_count));
            size -= unused_bit_count;
        }

        write_bits(buffer, stream_bits.bits);
        *buffer += 4;

        stream_bits.size = size;
        stream_bits.bits = bits << (bit_word_count - size);
    }
    else
    {
        stream_bits.bits |= (bits << (unused_bit_count - size));
        stream_bits.size += size;
    }
    
    return stream_bits;
}

STATIC_INLINE VLE PutBitsCoreWithSign(uint8_t** buffer, VLE stream_bits, uint32_t bits, uint_fast8_t size, bool positive )
{
    stream_bits = PutBitsCore( buffer, stream_bits, bits, size );
    
    BITCOUNT unused_bit_count = bit_word_count - stream_bits.size;
    
    if ( unused_bit_count == 0 )
    {
        write_bits(buffer, stream_bits.bits);
        *buffer += 4;
        
        stream_bits.size = 1;
        
        if( positive == false )
            stream_bits.bits = 1 << (bit_word_count - 1);
        else
            stream_bits.bits = 0;
    }
    else
    {
        stream_bits.size += 1;
        
        if( positive == false )
            stream_bits.bits |= (1 << (unused_bit_count - 1));
    }
    
    return stream_bits;
}

/*!
	@brief Encode the highpass band from the bitstream

	This routine does not encode runs of zeros across row boundaries.
*/
CODEC_ERROR EncodeHighpassBandRowRuns(BITSTREAM *stream, ENCODER_CODESET *codeset, PIXEL *data,
									  DIMENSION width, DIMENSION height, DIMENSION pitch)
{
	CODEC_ERROR error = CODEC_ERROR_OKAY;

	int row_padding;
	int row = 0;
	//int column = 0;
	//size_t index = 0;

	// The encoder uses the codebooks for magnitudes and runs of zeros
	const MAGS_TABLE *mags_table = codeset->mags_table;
	const RUNS_TABLE *runs_table = codeset->runs_table;
    int runs_table_length = runs_table->length;
	RLC *rlc = (RLC *)((uint8_t *)runs_table + sizeof(RUNS_TABLE));

	// The band is terminated by the band end codeword in the codebook
	const CODEBOOK *codebook = codeset->codebook;

	PIXEL *rowptr = data;
	int count = 0;

	// Convert the pitch to units of pixels
	assert((pitch % sizeof(PIXEL)) == 0);
	pitch /= sizeof(PIXEL);

	// Check that the band dimensions are reasonable
	assert(width <= pitch);

	// Compute the number of values of padding at the end of each row
	row_padding = pitch - width;

	VLE *mags_table_entry = (VLE *)((uint8_t *)mags_table + sizeof(MAGS_TABLE));

    VLE stream_bits;
    
    stream_bits.bits = stream->buffer;
    stream_bits.size = stream->count;
    
    struct _stream *bit_stream = stream->stream;
    
    int mags_table_length_minus_1 = mags_table->length - 1;
    
    uint8_t* stream_buffer      = (uint8_t *)bit_stream->location.memory.buffer + bit_stream->byte_count;
    uint8_t* stream_buffer_orig = stream_buffer;
    
	for (row = 0; row < height; row++)
	{
		int index = 0;			// Start at the beginning of the row

		// Search the row for runs of zeros and nonzero values
		while (1)
		{
			// Loop invariant
			assert(0 <= index && index < width);
            
            {
                PIXEL* start = rowptr + index;
                PIXEL* end   = rowptr + width;
                
                for (; *(start) == 0 && start != end; start++)
                {
                    
                }
                
                int x = start - (rowptr + index);
                    
                index += x;
                count += x;
            }
            
			// Need to output a value?
			if (index < width)
			{
                while (count > 0)
                {
                    if( count < 12 )
                    {
                        stream_bits = PutZeroBits(&stream_buffer, stream_bits, count );
                        break;
                    }
                    else
                    {
                        int count_index = minimum(count, runs_table_length - 1);
                        assert(count_index < runs_table->length);
                        
                        RLC rlc_val = rlc[count_index];
                        
                        stream_bits = PutBitsCore(&stream_buffer, stream_bits, rlc_val.bits, rlc_val.size );
                        
                        // Reduce the length of the run by the amount output
                        count -= rlc_val.count;
                    }
                }
                
				count = 0;
                
				// The value zero is run length coded and handled by another routine
				{
					PIXEL value = rowptr[index++];
					assert(value != 0);
                    
                    PIXEL abs_value = minimum( abs(value), mags_table_length_minus_1 );
                    
                    stream_bits = PutBitsCoreWithSign(&stream_buffer, stream_bits, mags_table_entry[abs_value].bits, mags_table_entry[abs_value].size, value > 0 );
				}
			}

			// Add the end of row padding to the encoded length
			if (index == width)
            {
                count += row_padding;
                break;
            }
		}

		// Should have processed the entire row
		assert(index == width);

		// Advance to the next row
		rowptr += pitch;
	}

	stream->count = stream_bits.size;
	stream->buffer = stream_bits.bits;
    bit_stream->byte_count += (stream_buffer - stream_buffer_orig);
    
	// // Need to output a pending run of zeros?
	if (count > 0)
	{
		error = PutZeros(stream, runs_table, count);
		if (error != CODEC_ERROR_OKAY) {
			return error;
		}
	}

	// Insert the special codeword that marks the end of the highpass band
	error = PutSpecial(stream, codebook, SPECIAL_MARKER_BAND_END);

	return error;
}

CODEC_ERROR PutVideoSubbandTrailer(ENCODER *encoder, BITSTREAM *stream)
{
    // Set the size of the large chunk for the highpass band codeblock
    PopSampleSize(stream);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Read the segment at the specified offset in the bitstream
 
	This routine is used to read a segment that was previously written at a previous
	location in the encoded sample.  This allows the encoder to update, rather than
	overwrite, a segment that has already been written.  Typically, this is done to
	insert the size or offset to a portion of the sample (syntax element) into a
	segment that acts as an index to the syntax element.
 */
CODEC_ERROR GetSampleOffsetSegment(BITSTREAM *bitstream, uint32_t offset, TAGVALUE *segment)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    uint32_t buffer;
    
    error = GetBlock(bitstream->stream, &buffer, sizeof(buffer), offset);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // Translate the segment to native byte order
    segment->longword = Swap32(buffer);
    
    // Cannot return a segment if the offset stack is empty
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Write the lowpass band header into the bitstream
 
	Each channel is encoded separately, so the lowpass band (subband zero)
	is the lowpass band in the wavelet at the highest level for each channel.
 
	The last element in the lowpass band header is a segment that contains the
	size of this subband.  The actual size is updated when the lowpass trailer
	is written (see @ref PutVideoLowpassTrailer).
 
	The lowpass start code is used to uniquely identify the start of the lowpass
	band header and is used by the decode to navigate to the next channel in the
	bitstream.
 
	@todo Consider writing a composite lowpass band for all channels with
	interleaved rows to facilitate access to the thumbnail image in the
	encoded sample.
 */
CODEC_ERROR PutVideoLowpassHeader(ENCODER *encoder, int channel_number, BITSTREAM *stream)
{
    CODEC_STATE *codec = &encoder->codec;
    PRECISION lowpass_precision = encoder->channel[channel_number].lowpass_precision;
    
    // Output the subband number
    if (codec->subband_number != 0)
    {
        PutTagPair(stream, CODEC_TAG_SubbandNumber, 0);
        codec->subband_number = 0;
    }
    
    // Output the lowpass precision
    //if (encoder->lowpass.precision != codec->lowpass.precision)
    if (lowpass_precision != codec->lowpass_precision)
    {
        PutTagPair(stream, CODEC_TAG_LowpassPrecision, lowpass_precision);
        codec->lowpass_precision = lowpass_precision;
    }
    
    // Write the chunk header for the codeblock
    PushSampleSize(stream, CODEC_TAG_LargeCodeblock);
    
    return CODEC_ERROR_OKAY;
}

