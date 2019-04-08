/*! @file decoder.h
 *
 *  @brief Implementation of core decoder functions and data structure
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

/*!
	@brief Align the bitstream to a byte boundary
 
	Enough bits are removed from the bitstream buffer to
	align the bitstream to the next byte.
 */
static CODEC_ERROR AlignBitsByte(BITSTREAM *bitstream)
{
    // Compute the number of bits to skip
    BITCOUNT count = bitstream->count % 8;
    GetBits(bitstream, count);
    assert((bitstream->count % 8) == 0);
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Align the bitstream to the next word boundary
 
	All of the bits in the bitstream buffer are flushed unless
	the bitstream buffer is completely empty or completely full.
 */
static CODEC_ERROR AlignBitsWord(BITSTREAM *bitstream)
{
    BITCOUNT count = bitstream->count;
    
    if (0 < count && count < bit_word_count) {
        GetBits(bitstream, count);
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Align the bitstream to the next tag value pair
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
    
    // Add the number of bytes read from the stream
    byte_count += stream->byte_count;
    
    while ((byte_count % sizeof(TAGVALUE)) != 0)
    {
        GetBits(bitstream, 8);
        byte_count++;
    }
    
    // The bitstream should be aligned to the next segment
    assert((bitstream->count == 0) || (bitstream->count == bit_word_count));
    assert((byte_count % sizeof(TAGVALUE)) == 0);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Initialize the decoder data structure
	This routine performs the same function as a C++ constructor.
	The decoder is initialized with default values that are replaced
	by the parameters used to prepare the decoder (see @ref PrepareDecoder).
	This routine does not perform all of the initializations required
	to prepare the decoder data structure for decoding a sample.
 */
CODEC_ERROR InitDecoder(DECODER *decoder, const gpr_allocator *allocator)
{
    assert(decoder != NULL);
    if (! (decoder != NULL)) {
        return CODEC_ERROR_NULLPTR;
    }
    
    memset(decoder, 0, sizeof(DECODER));
    
    // Assign a memory allocator to the decoder
    decoder->allocator = (gpr_allocator *)allocator;
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Release resources allocated by the decoder
	Note that this routine does not close the logfile.
 */
CODEC_ERROR ReleaseDecoder(DECODER *decoder)
{
    // Free the wavelet transforms and decoding buffers
    ReleaseDecoderTransforms(decoder);
    ReleaseDecoderBuffers(decoder);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Decode the bitstream encoded into the byte stream into separate component arrays
	This is a convenience routine for applications that use the byte stream data structure
	for bitstreams stored in a file or memory buffer.
	The main entry point for decoding a bitstream is @ref DecodingProcess.
	The parameters data structure is intended to simulate information that may be available
	to the decoder from the media container or an external application.
	This routine assumes that the unpacked image has already been initialized.
 */
CODEC_ERROR DecodeStream(STREAM *stream, UNPACKED_IMAGE *unpacked_image, const DECODER_PARAMETERS *parameters)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    BITSTREAM bitstream;
    DECODER decoder;
    
    // Initialize the bitstream data structure
    InitBitstream(&bitstream);
    
    // Bind the bitstream to the byte stream
    error = AttachBitstream(&bitstream, stream);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // Decode the bitstream sample into a image buffer
    error = DecodingProcess(&decoder, &bitstream, unpacked_image, parameters);
    
    // Release any resources allocated by the decoder
    ReleaseDecoder(&decoder);
    
    // Release any resources allocated by the bitstream
    ReleaseBitstream(&bitstream);
    
    return error;
}

/*!
	@brief Decode the bitstream encoded into the byte stream
	This is a convenience routine for applications that use the byte
	stream data structure for samples stored in a file or memory buffer.
	The main entry point for decoding a bitstream is @ref DecodingProcess.
	The parameters data structure is intended to simulate information that
	may be available to the decoder from the media container or an external
	application.
 */
CODEC_ERROR DecodeImage(STREAM *stream, IMAGE *packed_image, RGB_IMAGE *rgb_image, DECODER_PARAMETERS *parameters)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    BITSTREAM bitstream;
    DECODER decoder;
    DIMENSION packed_width;
    DIMENSION packed_height;
    PIXEL_FORMAT packed_format;
    
    SetupDecoderLogCurve();
    
    // The unpacked image will hold the component arrays decoded from the bitstream
    UNPACKED_IMAGE unpacked_image;
    
    // Initialize the bitstream data structure
    InitBitstream(&bitstream);
    
    // Bind the bitstream to the byte stream
    error = AttachBitstream(&bitstream, stream);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // The component arrays will be allocated after the bitstream is decoded
    InitUnpackedImage(&unpacked_image);
    
    // Decode the bitstream sample into a image buffer
    error = DecodingProcess(&decoder, &bitstream, &unpacked_image, parameters);
    
    if( error != CODEC_ERROR_OKAY )
    {
        return error;
    }

    switch (parameters->rgb_resolution) {

        case GPR_RGB_RESOLUTION_NONE:
            // The dimensions and format for the output of the image packing process
            SetOutputImageFormat(&decoder, parameters, &packed_width, &packed_height, &packed_format);

            // Allocate the image buffer for output of the image packing process
            AllocImage(decoder.allocator, packed_image, packed_width, packed_height, packed_format);
            
            // Pack the component arrays into the output image
            ImageRepackingProcess(&unpacked_image, packed_image, parameters);
            break;

        case GPR_RGB_RESOLUTION_HALF:
            WaveletToRGB(parameters->allocator, (PIXEL*)unpacked_image.component_array_list[0].data, (PIXEL*)unpacked_image.component_array_list[1].data, (PIXEL*)unpacked_image.component_array_list[2].data,
                         unpacked_image.component_array_list[2].width, unpacked_image.component_array_list[2].height, unpacked_image.component_array_list[2].pitch / 2,
                         rgb_image, 12, parameters->rgb_bits, &parameters->rgb_gain );
            break;
            
        case GPR_RGB_RESOLUTION_QUARTER:
            
            WaveletToRGB(parameters->allocator, decoder.transform[0].wavelet[0]->data[0], decoder.transform[1].wavelet[0]->data[0], decoder.transform[2].wavelet[0]->data[0],
                         decoder.transform[2].wavelet[0]->width, decoder.transform[2].wavelet[0]->height, decoder.transform[2].wavelet[0]->width,
                         rgb_image, 14, parameters->rgb_bits, &parameters->rgb_gain );
            break;
            
        case GPR_RGB_RESOLUTION_EIGHTH:
            
            WaveletToRGB(parameters->allocator, decoder.transform[0].wavelet[1]->data[0], decoder.transform[1].wavelet[1]->data[0], decoder.transform[2].wavelet[1]->data[0],
                         decoder.transform[2].wavelet[1]->width, decoder.transform[2].wavelet[1]->height, decoder.transform[2].wavelet[1]->width,
                         rgb_image, 14, parameters->rgb_bits, &parameters->rgb_gain );
            
            break;
            
        case GPR_RGB_RESOLUTION_SIXTEENTH:

            WaveletToRGB(parameters->allocator, decoder.transform[0].wavelet[2]->data[0], decoder.transform[1].wavelet[2]->data[0], decoder.transform[2].wavelet[2]->data[0],
                         decoder.transform[2].wavelet[2]->width, decoder.transform[2].wavelet[2]->height, decoder.transform[2].wavelet[2]->width,
                         rgb_image, 14, parameters->rgb_bits, &parameters->rgb_gain );
            break;
            
        default:
            return CODEC_ERROR_UNSUPPORTED_FORMAT;
            break;
    }
    
    ReleaseComponentArrays( &parameters->allocator, &unpacked_image, unpacked_image.component_count );
    
    // Release any resources allocated by the decoder
    ReleaseDecoder(&decoder);
    
    // Release any resources allocated by the bitstream
    ReleaseBitstream(&bitstream);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Initialize the decoder using the specified parameters
	@todo Add more error checking to this top-level routine
 */
CODEC_ERROR PrepareDecoder(DECODER *decoder, const DECODER_PARAMETERS *parameters)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Initialize the decoder data structure
    error = InitDecoder(decoder, &parameters->allocator);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // Set the mask that specifies which parts of the VC-5 standard are supported
    decoder->enabled_parts = parameters->enabled_parts;
    
    // Verify that the enabled parts are correct
    error = VerifyEnabledParts(decoder->enabled_parts);
    assert(error == CODEC_ERROR_OKAY);
    if (! (error == CODEC_ERROR_OKAY)) {
        return error;
    }
    
    // Initialize the codec state (allocation routines use the codec state)
    error = PrepareDecoderState(decoder, parameters);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    if (parameters != NULL)
    {
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
        decoder->layer_count = (uint_fast8_t)parameters->layer_count;
        decoder->progressive = parameters->progressive;
        decoder->top_field_first = parameters->top_field_first;
#endif
    }
    
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    if (IsPartEnabled(decoder->enabled_parts, VC5_PART_SECTIONS))
    {
        decoder->section_flag = parameters->section_flag;
    }
#endif
    
    decoder->subbands_to_decode = MAX_SUBBAND_COUNT;
    
    return error;
}

/*!
	@brief Decode a VC-5 bitstream to an ordered set of component arrays
	This is the main entry point for decoding a sample.  The decoder must
	have been initialized by a call to @ref PrepareDecoder.
	The bitstream must be initialized and bound to a byte stream before
	calling this routine.  The unpacked output image will be initialized by this
	routine to hold the decoded component arrays represented in the bitstream.
	@todo When the VC-5 part for layers is defined, should be able to pass a mask
	indicating which layers must be decoded
 */
CODEC_ERROR DecodingProcess(DECODER *decoder, BITSTREAM *stream, UNPACKED_IMAGE *image, const DECODER_PARAMETERS *parameters)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    TAGVALUE segment;
    
    // Initialize the decoder with a default allocator
    PrepareDecoder(decoder, parameters);
    
    // Get the bitstream start marker
    segment = GetSegment(stream);
    if (segment.longword != StartMarkerSegment)
    {
        return CODEC_ERROR_MISSING_START_MARKER;
    }

    // Set up number of subbands to decode
    if( parameters->rgb_resolution == GPR_RGB_RESOLUTION_SIXTEENTH )
    {
        decoder->subbands_to_decode = 1;
    }
    else if( parameters->rgb_resolution == GPR_RGB_RESOLUTION_EIGHTH )
    {
        decoder->subbands_to_decode = 4;
    }
    else if( parameters->rgb_resolution == GPR_RGB_RESOLUTION_QUARTER )
    {
        decoder->subbands_to_decode = 7;
    }
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    // Decode each layer in the sample
    if (decoder->layer_count > 1)
    {
        IMAGE decoded_image[MAX_LAYER_COUNT];
        PIXEL_FORMAT decoded_format = decoder->output.format;
        int layer_index;
        
        for (layer_index = 0; layer_index < decoder->layer_count; layer_index++)
        {
            DIMENSION layer_width = LayerWidth(decoder, decoder->output.width);
            DIMENSION layer_height = LayerHeight(decoder, decoder->output.height);
            
            // Allocate a image for this layer
            AllocImage(decoder->allocator, &decoded_image[layer_index], layer_width, layer_height, decoded_format);
            
            // Decode the layer into its own image
            error = DecodeSampleLayer(decoder, stream, &decoded_image[layer_index]);
            if (error != CODEC_ERROR_OKAY) {
                break;
            }
        }
        
        if (error == CODEC_ERROR_OKAY)
        {
            // The decoded image in each layer is composited into the output image
            error = ReconstructSampleFrame(decoder, decoded_image, decoder->layer_count, image);
        }
        
        // Free the images used for decoding each layer
        for (layer_index = 0; layer_index < decoder->layer_count; layer_index++)
        {
            ReleaseImage(decoder->allocator, &decoded_image[layer_index]);
        }
    }
    else
#endif
    {
        // A VC-5 Part 1 bitstream can only contain a single layer (encoded image)
        error = DecodeSingleImage(decoder, stream, image, parameters);
    }
    
    // Done decoding all layers in the sample and computing the output image
    return error;
}

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Decode the portion of a sample that corresponds to a single layer
	Samples can be contain multiple subsamples.  Each subsample may correspond to
	a different view.  For example, an encoded video sample may contain both the
	left and right subsamples in a stereo pair.
	Subsamples have been called tracks or channels, but this terminology can be
	confused with separate video tracks in a multimedia container or the color
	planes that are called channels elsewhere in this codec.
	The subsamples are decoded seperately and composited to form a single image
	that is the output of the complete process of decoding a single video sample.
	For this reason, the subsamples are called layers.
	@todo Okay to call a subsample a layer?
 */
CODEC_ERROR DecodeSampleLayer(DECODER *decoder, BITSTREAM *input, UNPACKED_IMAGE *image)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Initialize the codec state (including the dimensions of the first wavelet band)
    PrepareDecoderState(decoder, NULL);
    
    // Reset the flags in the wavelet transforms
    PrepareDecoderTransforms(decoder);
    
    // Process tag value pairs until the layer has been decoded
    for (;;)
    {
        TAGVALUE segment;
        
        // Read the next tag value pair from the bitstream
        segment = GetSegment(input);
        assert(input->error == BITSTREAM_ERROR_OKAY);
        if (input->error != BITSTREAM_ERROR_OKAY) {
            decoder->error = CodecErrorBitstream(input->error);
            return decoder->error;
            break;
        }
        
        error = UpdateCodecState(decoder, input, segment);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Processed all wavelet bands in all channels?
        if (IsLayerComplete(decoder)) break;
        
    }
    
    // Parsed the bitstream without errors?
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // Reconstruct the output image using the last decoded wavelet in each channel
    return ReconstructLayerImage(decoder, image);
}
#endif

/*!
	@brief Decode the bitstream into a list of component arrays
 */
CODEC_ERROR DecodeSingleImage(DECODER *decoder, BITSTREAM *input, UNPACKED_IMAGE *image, const DECODER_PARAMETERS *parameters)
{
    TIMESTAMP("[BEG]", 2)
    
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    CODEC_STATE *codec = &decoder->codec;
    
    // Process tag value pairs until the layer has been decoded
    for (;;)
    {
        TAGVALUE segment;
        
        // Read the next tag value pair from the bitstream
        segment = GetSegment(input);
        assert(input->error == BITSTREAM_ERROR_OKAY);
        if (input->error != BITSTREAM_ERROR_OKAY) {
            decoder->error = CodecErrorBitstream(input->error);
            return decoder->error;
            break;
        }
        
        error = UpdateCodecState(decoder, input, segment);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Processed all wavelet bands in all channels?
        if ( IsDecodingComplete(decoder) && codec->header == false ) {
            break;
        }
    }
    
    // Parsed the bitstream without errors?
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    TIMESTAMP("[END]", 2)
    
    if( parameters->rgb_resolution == GPR_RGB_RESOLUTION_NONE ||
        parameters->rgb_resolution == GPR_RGB_RESOLUTION_HALF ||
        parameters->rgb_resolution == GPR_RGB_RESOLUTION_FULL )
    {
        // Reconstruct the output image using the last decoded wavelet in each channel
        error = ReconstructUnpackedImage(decoder, image);
    }
    
    return error;
}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
/*!
	@brief Set the channel dimensions from the image dimensions and format
	This routine is used set the channel dimensions and other channel-specific
	parameters after the bitstream header has been parsed.  The dimensions of
	each channel can only be set using the parameters present in the bitstream
	header if the bitstream conforms to VC-5 Part 3.
 */
CODEC_ERROR SetImageChannelParameters(DECODER *decoder, int channel_number)
{
    CODEC_STATE *codec = &decoder->codec;
    IMAGE_FORMAT image_format = codec->image_format;
    DIMENSION image_width = codec->image_width;
    DIMENSION image_height = codec->image_height;
    DIMENSION pattern_width = codec->pattern_width;
    DIMENSION pattern_height = codec->pattern_height;
    
    // Are the image dimensions valid?
    if (image_width == 0 || image_height == 0)
    {
        // Cannot set the channel dimensions without valid image dimensions
        return CODEC_ERROR_IMAGE_DIMENSIONS;
    }
    
    // Are the pattern dimensions valid?
    if (pattern_width == 0 || pattern_height == 0)
    {
        // The channel dimensions may depend on the pattern dimensions
        return CODEC_ERROR_PATTERN_DIMENSIONS;
    }
    
    switch (image_format)
    {            
        case IMAGE_FORMAT_RAW:
            // The pattern width and height must be two
            assert(pattern_width == 2 && pattern_height == 2);
            
            // The image dimensions must be divisible by the pattern dimensions
            //assert((image_width % 2) == 0 && (image_height % 2) == 0);
            
            decoder->channel[channel_number].width = image_width / 2;
            decoder->channel[channel_number].height = image_height / 2;
            break;

        default:
            // Cannot set the channel dimensions without a valid image format
            return CODEC_ERROR_BAD_IMAGE_FORMAT;
            break;
    }
    
    //TODO: Is the default bits per component the correct value to use?
    decoder->channel[channel_number].bits_per_component = codec->bits_per_component;
    decoder->channel[channel_number].initialized = true;
    
    return CODEC_ERROR_OKAY;
}
#endif

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
/*!
	@brief Allocate all of the wavelets used during decoding
	This routine allocates all of the wavelets in the wavelet tree that
	may be used during decoding.
	This routine is used to preallocate the wavelets before decoding begins.
	If the wavelet bands are allocated on demand if not preallocated.
	By default, the wavelet bands are encoded into the bitstream with the bands
	from the wavelet at the highest level (smallest wavelet) first so that the
	bands can be processed by the decoder in the order as the sample is decoded.
 */
CODEC_ERROR AllocDecoderTransforms(DECODER *decoder)
{
    CODEC_ERROR result;
    // Use the default allocator for the decoder
    
    gpr_allocator *allocator = decoder->allocator;
    int channel_number;
    int wavelet_index;
    
    int channel_count;
    int wavelet_count;
    
    assert(decoder != NULL);
    if (! (decoder != NULL)) {
        return CODEC_ERROR_NULLPTR;
    }
    
    channel_count = decoder->codec.channel_count;
    wavelet_count = decoder->wavelet_count;
    
    for (channel_number = 0; channel_number < channel_count; channel_number++)
    {
        DIMENSION wavelet_width;
        DIMENSION wavelet_height;
        
        // Set the channel dimensions using the information obtained from the bitstream header
        result = SetImageChannelParameters(decoder, channel_number);
        if( result != CODEC_ERROR_OKAY )
        {
            assert(0);
            return result;
        }
        
        // Check that the channel dimensions and other parameters have been set
        assert(decoder->channel[channel_number].initialized);
        
        // The dimensions of the wavelet at level zero are equal to the channel dimensions
        wavelet_width = decoder->channel[channel_number].width;
        wavelet_height = decoder->channel[channel_number].height;
        
        for (wavelet_index = 0; wavelet_index < wavelet_count; wavelet_index++)
        {
            WAVELET *wavelet;
            
            // Pad the wavelet width if necessary
            if ((wavelet_width % 2) != 0) {
                wavelet_width++;
            }
            
            // Pad the wavelet height if necessary
            if ((wavelet_height % 2) != 0) {
                wavelet_height++;
            }
            
            // Dimensions of the current wavelet must be divisible by two
            assert((wavelet_width % 2) == 0 && (wavelet_height % 2) == 0);
            
            // Reduce the dimensions of the next wavelet by half
            wavelet_width /= 2;
            wavelet_height /= 2;
            
            wavelet = CreateWavelet(allocator, wavelet_width, wavelet_height);
            decoder->transform[channel_number].wavelet[wavelet_index] = wavelet;
        }
    }
    
    return CODEC_ERROR_OKAY;
}
#endif

/*!
	@brief Free the wavelet transforms allocated by the decoder
 */
CODEC_ERROR ReleaseDecoderTransforms(DECODER *decoder)
{
    int channel_count = decoder->codec.channel_count;
    int channel_index;
    
    for (channel_index = 0; channel_index < channel_count; channel_index++)
    {
        int wavelet_index;
        
        for (wavelet_index = 0; wavelet_index < decoder->wavelet_count; wavelet_index++)
        {
            WAVELET *wavelet = decoder->transform[channel_index].wavelet[wavelet_index];
            DeleteWavelet(decoder->allocator, wavelet);
        }
    }
    
    return CODEC_ERROR_OKAY;
}

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
/*!
	@brief Allocate all of the buffers required for decoding
	This routine allocates buffers required for decoding, not including
	the wavelet images in the wavelet tree which are allocated by
	@ref AllocDecoderTransforms
	This routine is used to preallocate buffers before decoding begins.
	Decoding buffers are allocated on demand if not preallocated.
	Currently, the reference decoder allocates scratch buffers as required
	by each routine that needs scratch space and the scratch buffers are
	deallocated at the end each routine that allocates scratch space.
	@todo Should it be an error if the buffers are not preallocated?
 */
CODEC_ERROR AllocDecoderBuffers(DECODER *decoder)
{
    (void)decoder;
    return CODEC_ERROR_UNIMPLEMENTED;
}
#endif

/*!
	@brief Free any buffers allocated by the decoder
 */
CODEC_ERROR ReleaseDecoderBuffers(DECODER *decoder)
{
    (void)decoder;
    return CODEC_ERROR_UNIMPLEMENTED;
}

/*!
	@brief Allocate the wavelets for the specified channel
 */
CODEC_ERROR AllocateChannelWavelets(DECODER *decoder, int channel_number)
{
    // Use the default allocator for the decoder
    gpr_allocator *allocator = decoder->allocator;
    int wavelet_index;
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    // Use the channel dimensions computed from the image dimension and image format
    DIMENSION channel_width = decoder->channel[channel_number].width;
    DIMENSION channel_height = decoder->channel[channel_number].height;
#else
    // Use the channel dimensions from the current codec state
    DIMENSION channel_width = decoder->codec.channel_width;
    DIMENSION channel_height = decoder->codec.channel_height;
#endif
    
    // Round up the wavelet dimensions to an even number
    DIMENSION wavelet_width = ((channel_width % 2) == 0) ? channel_width / 2 : (channel_width + 1) / 2;
    DIMENSION wavelet_height = ((channel_height % 2) == 0) ? channel_height / 2 : (channel_height + 1) / 2;
    
    //TODO: Check for errors before the code that initializes the local variables
    assert(decoder != NULL);
    if (! (decoder != NULL)) {
        return CODEC_ERROR_NULLPTR;
    }
    
    for (wavelet_index = 0; wavelet_index < decoder->wavelet_count; wavelet_index++)
    {
        WAVELET *wavelet = decoder->transform[channel_number].wavelet[wavelet_index];
        
        // Has a wavelet already been created?
        if (wavelet != NULL)
        {
            // Is the wavelet the correct size?
            if (wavelet_width != wavelet->width ||
                wavelet_height != wavelet->height)
            {
                // Deallocate the wavelet
                DeleteWavelet(allocator, wavelet);
                
                wavelet = NULL;
            }
        }
        
        if (wavelet == NULL)
        {
            wavelet = CreateWavelet(allocator, wavelet_width, wavelet_height);
            assert(wavelet != NULL);
            
            decoder->transform[channel_number].wavelet[wavelet_index] = wavelet;
        }
        
        // Pad the wavelet width if necessary
        if ((wavelet_width % 2) != 0) {
            wavelet_width++;
        }
        
        // Pad the wavelet height if necessary
        if ((wavelet_height % 2) != 0) {
            wavelet_height++;
        }
        
        // Dimensions of the current wavelet must be divisible by two
        assert((wavelet_width % 2) == 0 && (wavelet_height % 2) == 0);
        
        // Reduce the dimensions of the next wavelet by half
        wavelet_width /= 2;
        wavelet_height /= 2;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Initialize the codec state before starting to decode a bitstream
 */
CODEC_ERROR PrepareDecoderState(DECODER *decoder, const DECODER_PARAMETERS *parameters)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    CODEC_STATE *codec = &decoder->codec;
    
    // Set the parameters that control the decoding process
    decoder->wavelet_count = 3;
    
    // The wavelets and decoding buffers have not been allocated
    decoder->memory_allocated = false;
    
    // Clear the table of information about each decoded channel
    memset(decoder->channel, 0, sizeof(decoder->channel));
    
    // Set the codebook
    decoder->codebook = (CODEBOOK *)decoder_codeset_17.codebook;
    
    // Initialize the codec state with the default parameter values
    error = PrepareCodecState(codec);
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // Initialize the codec state with the external parameter values
    codec->image_width = parameters->input.width;
    codec->image_height = parameters->input.height;
    
    //TODO: Initialize other parameters with external values?
    
    // The default channel dimensions are the image dimensions
    codec->channel_width = codec->image_width;
    codec->channel_height = codec->image_height;
    
    return error;
}

/*!
	@brief Prepare the decoder transforms for the next layer
	Each wavelet in the decoder transforms contain flags that indicate
	whether the wavelet bands must be decoded.  These flags must be reset
	before decoding the next layer.
 */
CODEC_ERROR PrepareDecoderTransforms(DECODER *decoder)
{
    int channel_count = decoder->codec.channel_count;
    int channel_index;
    
    for (channel_index = 0; channel_index < channel_count; channel_index++)
    {
        int wavelet_count = decoder->wavelet_count;
        int wavelet_index;
        
        for (wavelet_index = 0; wavelet_index < wavelet_count; wavelet_index++)
        {
            WAVELET *wavelet = decoder->transform[channel_index].wavelet[wavelet_index];
            wavelet->valid_band_mask = 0;
        }
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Pack the component arrays into the output image
	
	The decoding process outputs a set of component arrays that does not correspond
	to any common image format.  The image repacking process converts the ordered
	set of component arrays output by the decoding processing into a packed image.
	The image repacking process is not normative in VC-5 Part 1.
 */
CODEC_ERROR ImageRepackingProcess(const UNPACKED_IMAGE *unpacked_image,
                                  PACKED_IMAGE *packed_image,
                                  const DECODER_PARAMETERS *parameters)
{
    DIMENSION output_width = packed_image->width;
    DIMENSION output_height = packed_image->height;
    size_t output_pitch = packed_image->pitch;
    PIXEL_FORMAT output_format = packed_image->format;
    PIXEL *output_buffer = packed_image->buffer;
    ENABLED_PARTS enabled_parts = parameters->enabled_parts;
    
    (void)parameters;

    // The dimensions must be in units of Bayer pattern elements
    output_width /= 2;
    output_height /= 2;
    output_pitch *= 2;

    switch (output_format)
    {
        case PIXEL_FORMAT_RAW_RGGB_12:
        case PIXEL_FORMAT_RAW_GBRG_12:
            return PackComponentsToRAW(unpacked_image, output_buffer, output_pitch,
                                        output_width, output_height, enabled_parts, 12, output_format );
            
        case PIXEL_FORMAT_RAW_RGGB_14:
        case PIXEL_FORMAT_RAW_GBRG_14:
            return PackComponentsToRAW(unpacked_image, output_buffer, output_pitch,
                                        output_width, output_height, enabled_parts, 14, output_format );
            break;

		case PIXEL_FORMAT_RAW_RGGB_16:
		case PIXEL_FORMAT_RAW_GBRG_16:
            return PackComponentsToRAW(unpacked_image, output_buffer, output_pitch,
                                        output_width, output_height, enabled_parts, 16, output_format );
            break;
            
        default:
            assert(0);
            break;
    }
    
    // Unsupported output image format
    return CODEC_ERROR_UNSUPPORTED_FORMAT;
}

/*!
	@brief Compute default parameters for the repacked image
 */
CODEC_ERROR SetOutputImageFormat(DECODER *decoder,
                                 const DECODER_PARAMETERS *parameters,
                                 DIMENSION *width_out,
                                 DIMENSION *height_out,
                                 PIXEL_FORMAT *format_out)
{
    // The image dimensions are in units of samples
    DIMENSION output_width = decoder->codec.image_width;
    DIMENSION output_height = decoder->codec.image_height;
    
    PIXEL_FORMAT output_format = PIXEL_FORMAT_UNKNOWN;
    
    // Override the pixel format with the format passed as a parameter
    if (parameters->output.format != PIXEL_FORMAT_UNKNOWN) {
        output_format = parameters->output.format;
    }
    assert(output_format != PIXEL_FORMAT_UNKNOWN);
    
    if (width_out != NULL) {
        *width_out = output_width;
    }
    
    if (height_out != NULL) {
        *height_out = output_height;
    }
    
    if (format_out != NULL) {
        *format_out = output_format;
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Return true if the lowpass bands in all channels are valid
 */
bool ChannelLowpassBandsAllValid(const DECODER *decoder, int index)
{
    int channel_count = decoder->codec.channel_count;
    int channel;
    for (channel = 0; channel < channel_count; channel++)
    {
        WAVELET *wavelet = decoder->transform[channel].wavelet[index];
        if ((wavelet->valid_band_mask & BandValidMask(0)) == 0) {
            return false;
        }
    }
    
    // All channels have valid lowpass bands at the specified level
    return true;
}

#if VC5_ENABLED_PART(VC5_PART_SECTIONS)

/*
 @brief Return true if the tag identifies a section header
 */
bool IsSectionHeader(TAGWORD tag)
{
    switch (tag)
    {
        case CODEC_TAG_ImageSectionTag:
        case CODEC_TAG_HeaderSectionTag:
        case CODEC_TAG_LayerSectionTag:
        case CODEC_TAG_ChannelSectionTag:
        case CODEC_TAG_WaveletSectionTag:
        case CODEC_TAG_SubbandSectionTag:
            return true;
            
        default:
            return false;
    }
    
    return false;
}

/*
 @brief Map the tag for a section header to the section number
 */
CODEC_ERROR GetSectionNumber(TAGWORD tag, int *section_number_out)
{
    int section_number = 0;
    
    switch (tag)
    {
        case CODEC_TAG_ImageSectionTag:
            section_number = 1;
            break;
            
        case CODEC_TAG_HeaderSectionTag:
            section_number = 2;
            break;
            
        case CODEC_TAG_LayerSectionTag:
            section_number = 3;
            break;
            
        case CODEC_TAG_ChannelSectionTag:
            section_number = 4;
            break;
            
        case CODEC_TAG_WaveletSectionTag:
            section_number = 5;
            break;
            
        case CODEC_TAG_SubbandSectionTag:
            section_number = 6;
            break;
            
        default:
            assert(0);
            break;
    }
    
    if (section_number_out != NULL) {
        *section_number_out = section_number;
    }
    
    if (section_number > 0) {
        return CODEC_ERROR_OKAY;
    }
    
    return CODEC_ERROR_BAD_SECTION_TAG;
}

/*!
 @brief Write section information to the section log file
 */
CODEC_ERROR WriteSectionInformation(FILE *logfile, int section_number, int section_length)
{
    fprintf(logfile, "Section: %d, length: %d\n", section_number, section_length);
    return CODEC_ERROR_OKAY;
}

#endif

/*!
	@brief Skip the payload in a chunk
 
	A chunk is a tag value pair where the value specifies the length
	of a payload.  If the tag is a negative number, then the payload
	can be skipped without affecting the decoding process.
 */
static CODEC_ERROR SkipPayload(BITSTREAM *bitstream, int chunk_size)
{
    // The chunk size is in units of 32-bit words
    size_t size = 4 * chunk_size;
    
    // This routine assumes that the bit buffer is empty
    assert(bitstream->count == 0);
    
    // Skip the specified number of bytes in the stream
    return SkipBytes(bitstream->stream, size);
}

/*!
 @brief Parse the unique image identifier in a small chunk payload
 
 @todo Should the UMID instance number be a parameter to this routine?
 */
static CODEC_ERROR ParseUniqueImageIdentifier(DECODER *decoder, BITSTREAM *stream, size_t identifier_length)
{
    const int UMID_length_byte = 0x13;
    const int UMID_instance_number = 0;
    
    // Total length of the unique image identifier chunk payload (in segments)
    const int identifier_chunk_payload_length = UMID_length + sequence_number_length;
    
    uint8_t byte_array[12];
    BITWORD length_byte;
    BITWORD instance_number;
    
    // Check that the chunk payload has the correct length (in segments)
    if (identifier_length != identifier_chunk_payload_length) {
        return CODEC_ERROR_SYNTAX_ERROR;
    }
    
    // The unique image identifier chunk should begin with a UMID label
    GetByteArray(stream, byte_array, sizeof(byte_array));
    if (memcmp(byte_array, UMID_label, sizeof(UMID_label)) != 0) {
        return CODEC_ERROR_UMID_LABEL;
    }
    
    // Check the UMID length byte
    length_byte = GetBits(stream, 8);
    if (length_byte != UMID_length_byte) {
        return CODEC_ERROR_SYNTAX_ERROR;
    }
    
    // Check the UMID instance number
    instance_number = GetBits(stream, 24);
    if (instance_number != UMID_instance_number) {
        return CODEC_ERROR_SYNTAX_ERROR;
    }
    
    // Read the image sequence identifier
    GetByteArray(stream, decoder->image_sequence_identifier, sizeof(decoder->image_sequence_identifier));
    
    // Read the image sequence number
    decoder->image_sequence_number = GetBits(stream, 32);
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Update the codec state with the specified tag value pair
	When a segment (tag value pair) is encountered in the bitstream of an
	encoded sample, it may imply some change in the codec state.  For example,
	when a tag for the encoded format is read from the bitstream, the encoded
	format entry in the codec state may change.
	Some tags require that additional information must be read from the
	bitstream and more segments may be encountered, leading to additional
	changes in the codec state.
	A tag may identify a single parameter and the parameter value must be updated
	in the codec state with the new value specified in the segment, but a tag may
	also imply that other pparameter values must be updated.  For example, the tag
	that marks the first encounter with a wavelet at a lower level in the wavelet
	tree implies that the width and height of wavelet bands that may be encoded in
	the remainder of the sample must be doubled.
	
	It is not necessary for the encoder to insert segments into the bitstream if the
	codec state change represented by an encoded tag and value can be deduced from
	earlier segments in the bitstream and the codec state can be changed at a time
	during decoding that is functionally the same as when the state change would have
	been performed by an explicitly encoded tag and value.
	@todo Need to check that parameters found in the sample are consistent with
	the decoding parameters used to initialize the codec state.
 */
CODEC_ERROR UpdateCodecState(DECODER *decoder, BITSTREAM *stream, TAGVALUE segment)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    CODEC_STATE *codec = &decoder->codec;
    ENABLED_PARTS enabled_parts = decoder->enabled_parts;
    bool optional = false;
    int chunk_size = 0;
    TAGWORD tag = segment.tuple.tag;
    TAGWORD value = segment.tuple.value;
    
    // The enabled parts variable may not be used depending on the compile-time options
    (void)enabled_parts;
    
    // Assume that the next syntax element is not a tag-value pair for a header parameter
    codec->header = false;
    
    // Assume that the next syntax element is not a codeblock (large chunk element)
    codec->codeblock = false;
    
    // Is this an optional tag?
    if (tag < 0) {
        tag = RequiredTag(tag);
        optional = true;
    }
    
    switch (tag)
    {
        case CODEC_TAG_ChannelCount:		// Number of channels in the transform
            assert(0 < value && value <= MAX_CHANNEL_COUNT);
            codec->channel_count = (uint_least8_t)value;
            codec->header = true;
            break;
            
        case CODEC_TAG_ImageWidth:			// Width of the image
            codec->image_width = value;
            codec->header = true;
            
            // The image width is the default width of the next channel in the bitstream
            codec->channel_width = value;
            break;
            
        case CODEC_TAG_ImageHeight:			// Height of the image
            codec->image_height = value;
            codec->header = true;
            
            // The image height is the default height of the next channel in the bitstream
            codec->channel_height = value;
            break;
            
        case CODEC_TAG_SubbandNumber:		// Subband number of this wavelet band
            codec->subband_number = value;
            break;
            
        case CODEC_TAG_Quantization:		// Quantization applied to band
            codec->band.quantization = value;
            break;
            
        case CODEC_TAG_LowpassPrecision:	// Number of bits per lowpass coefficient
            if (! (PRECISION_MIN <= value && value <= PRECISION_MAX)) {
                return CODEC_ERROR_LOWPASS_PRECISION;
            }
            codec->lowpass_precision = (PRECISION)value;
            break;
            
        case CODEC_TAG_ChannelNumber:		// Channel number
            codec->channel_number = value;
            break;
            
        case CODEC_TAG_BitsPerComponent:	// Number of bits in the video source
            codec->bits_per_component = (PRECISION)value;
            //error = SetDecoderBitsPerComponent(decoder, codec->channel_number, codec->bits_per_component);
            break;
            
        case CODEC_TAG_PrescaleShift:
            UpdatePrescaleTable(codec, value);
            break;
            
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_ImageFormat:
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                codec->image_format = (IMAGE_FORMAT)value;
                codec->header = true;
            }
            else
            {
                // The image format shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            break;
            
        case CODEC_TAG_PatternWidth:
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                codec->pattern_width = (DIMENSION)value;
                codec->header = true;
            }
            else
            {
                // The pattern width shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            break;
            
        case CODEC_TAG_PatternHeight:
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                codec->pattern_height = (DIMENSION)value;
                codec->header = true;
            }
            else
            {
                // The pattern height shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            break;
            
        case CODEC_TAG_ComponentsPerSample:
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                codec->components_per_sample = (DIMENSION)value;
                codec->header = true;
            }
            else
            {
                // The components per sample shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            break;
            
        case CODEC_TAG_MaxBitsPerComponent:
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                codec->max_bits_per_component = (PRECISION)value;
                codec->header = true;
            }
            else
            {
                // The components per sample shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            break;
#endif
            
        case CODEC_TAG_ChannelWidth:
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                // The channel width shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            else
#endif
            {
                // The channel width may be present in the bitstream
                codec->channel_width = (DIMENSION)value;
            }
            break;
            
        case CODEC_TAG_ChannelHeight:
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
            if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS))
            {
                // The channel height shall not be present in the bitstream
                assert(0);
                error = CODEC_ERROR_BITSTREAM_SYNTAX;
            }
            else
#endif
            {
                // The channel height may be present in the bitstream
                codec->channel_height = (DIMENSION)value;
            }
            break;
            
        default:		// Unknown tag or the tag identifies a chunk
            
            //TODO: Check for chunk tags that are not defined in VC-5 Part 1
            
            // Does this tag indicate a chunk of data?
            if (tag & CODEC_TAG_CHUNK_MASK)
            {
                // Does this chunk have a 24-bit size?
                if(tag & CODEC_TAG_LARGE_CHUNK)
                {
                    // The chunk size includes the low byte in the tag
                    chunk_size = (value & 0xFFFF);
                    chunk_size += ((tag & 0xFF) << 16);
                }
                else
                {
                    // The chunk size is specified by the value
                    chunk_size = (value & 0xFFFF);
                }
            }
            
            // Is this a codeblock?
            if ((tag & CODEC_TAG_LargeCodeblock) == CODEC_TAG_LargeCodeblock)
            {
                codec->codeblock = true;
            }
            
            // Is this chunk a unique image identifier?
            else if (tag == CODEC_TAG_UniqueImageIdentifier)
            {
                // The unique image identifier should be optional
                assert(optional);
                if (! optional) {
                    return CODEC_ERROR_SYNTAX_ERROR;
                }
                
                // Parse the unique image identifier
                error = ParseUniqueImageIdentifier(decoder, stream, chunk_size);
            }
            
            // Is this chunk an inverse component transform?
            else if (tag == CODEC_TAG_InverseTransform)
            {
                // The inverse component transform should not be optional
                assert(!optional);
                if (optional) {
                    return CODEC_ERROR_SYNTAX_ERROR;
                }
                
                // Parse the inverse component transform
                error = ParseInverseComponentTransform(decoder, stream, chunk_size);
            }
            
            // Is this chunk an inverse component permutation?
            else if (tag == CODEC_TAG_InversePermutation)
            {
                // The inverse component permutation should not be optional
                assert(!optional);
                if (optional) {
                    return CODEC_ERROR_SYNTAX_ERROR;
                }
                
                // Parse the inverse component permutation
                error = ParseInverseComponentPermutation(decoder, stream, chunk_size);
            }
            
            // Is this chunk a 16-bit inverse component transform?
            else if (tag == CODEC_TAG_InverseTransform16)
            {
                // The 16-bit inverse component transform is not supported
                assert(0);
                return CODEC_ERROR_UNIMPLEMENTED;
            }
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
            // Is this a section header?
            else if (IsPartEnabled(enabled_parts, VC5_PART_SECTIONS) && decoder->section_flag && IsSectionHeader(tag))
            {
                int section_number;
                
                // Section headers are optional tag-value pairs
                optional = true;
                
                // Is this a bitstream header section?
                if (tag == CODEC_TAG_HeaderSectionTag)
                {
                    // Handle this tag-value pair as if it was a bitstream header parameter
                    codec->header = true;
                }
                
                // Convert the tag to a section number
                GetSectionNumber(tag, &section_number);
                
                // Record the section number and length (in segments)
                codec->section_number = section_number;
                codec->section_length = chunk_size;
                
                if( decoder->section_logfile )
                {
                    // Write the section information to the log file
                    WriteSectionInformation(decoder->section_logfile, section_number, chunk_size);                    
                }
            }
#endif
            else
            {
                // Does this chunk have a 24-bit chunk payload size?
                if (tag & CODEC_TAG_LARGE_CHUNK)
                {
                    optional = true;
                    chunk_size = 0;
                }
                
                assert(optional);
                if (!optional)
                {
                    error = CODEC_ERROR_BITSTREAM_SYNTAX;
                }
                else if (chunk_size > 0)
                {
                    // Skip processing the payload of this optional chunk element
                    SkipPayload(stream, chunk_size);
                }
            }
            break;
    }
    
    // Encountered an error while processing the tag?
    if (error != CODEC_ERROR_OKAY)
    {
        return error;
    }
    
    //TODO: Check that bitstreams with missplaced header parameters fail to decode
    
    //if (IsHeaderParameter(tag))
    if (codec->header)
    {
        if (optional)
        {
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
            if (tag == CODEC_TAG_HeaderSectionTag)
            {
                // Okay for the bitstream header to contain an optional section header tag-value pair
            }
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
            else if (!IsPartEnabled(enabled_parts, VC5_PART_LAYERS))
            {
                // A header parameter cannot be optional
                error =  CODEC_ERROR_REQUIRED_PARAMETER;
            }
#endif
#endif
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
            if (!IsPartEnabled(enabled_parts, VC5_PART_LAYERS))
            {
                // A header parameter cannot be optional
                error =  CODEC_ERROR_REQUIRED_PARAMETER;
            }
#endif
        }
        else if (decoder->header_finished)
        {
            // Should not encounter a header parameter after the header has been parsed
            error = CODEC_ERROR_BITSTREAM_SYNTAX;
        }
        else
        {
            // Record that this header parameter has been decoded
            error = UpdateHeaderParameter(decoder, tag);
        }
    }
    else if (!decoder->header_finished)
    {
        // There should be no more header parameters in the bitstream
        decoder->header_finished = true;
    }
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    // The wavelets and buffers can be allocated after the bitstream header has been parsed
    if (IsPartEnabled(enabled_parts, VC5_PART_IMAGE_FORMATS) &&
        decoder->header_finished &&
        !decoder->memory_allocated)
    {
        // Allocate space for the wavelet transforms
        AllocDecoderTransforms(decoder);
        
        // Allocate all buffers required for decoding
        AllocDecoderBuffers(decoder);
        
        // Reset the flags in the wavelet transforms
        PrepareDecoderTransforms(decoder);
        
        // The wavelet transforms and decoding buffers have been allocated
        decoder->memory_allocated = true;
    }
#endif
    
    // Found a codeblock element?
    if (codec->codeblock)
    {
        const int channel_number = codec->channel_number;
        
        // Have the channel dimensions been initialized?
        if (!decoder->channel[channel_number].initialized)
        {
            // Record the channel dimensions and component precision
            decoder->channel[channel_number].width = codec->channel_width;
            decoder->channel[channel_number].height = codec->channel_height;
            
            // Initialize the dimensions of this channel
            decoder->channel[channel_number].initialized = true;
            
            //TODO: Allocate space for the wavelet transforms and decoding buffers
        }
        
        // Is this the first codeblock encountered in the bitstream for this channel?
        if (!decoder->channel[channel_number].found_first_codeblock)
        {
            // Remember the number of bits per component in this and higher numbered channel
            decoder->channel[codec->channel_number].bits_per_component = codec->bits_per_component;
            
            // Found the first codeblock in the channel
            decoder->channel[channel_number].found_first_codeblock = true;
        }
        
        {
            CODEC_STATE *codec = &decoder->codec;
            
            const int subband_number = codec->subband_number;
            
            if( subband_number <  decoder->subbands_to_decode )
            {
                // Decode the subband into its wavelet band
                error = DecodeChannelSubband(decoder, stream, chunk_size);
            }
            else
            {
                // Skip decoding of subband
                error = SkipPayload(stream, chunk_size);
                
                WAVELET* wavelet = decoder->transform[channel_number].wavelet[SubbandWaveletIndex(subband_number)];
                wavelet->valid_band_mask = 0xF;
            }
            
            // Set the subband number for the next band expected in the bitstream
            codec->subband_number++;
            
            // Was the subband successfully decoded?
            if (error == CODEC_ERROR_OKAY)
            {
                // Record that this subband has been decoded successfully
                SetDecodedBandMask(codec, subband_number);
            }
            
            // Done decoding all subbands in this channel?
            if (codec->subband_number == codec->subband_count)
            {
                // Advance to the next channel
                codec->channel_number++;
                
                // Reset the subband number
                codec->subband_number = 0;
            }
        }
        
    }
    
    return error;
}

/*!
	@brief Return true if the tag corresponds to a bitstream header parameter
 */
bool IsHeaderParameter(TAGWORD tag)
{
    switch (tag)
    {
        case CODEC_TAG_ImageWidth:
        case CODEC_TAG_ImageHeight:
        case CODEC_TAG_ChannelCount:
        case CODEC_TAG_SubbandCount:
            
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_ImageFormat:
        case CODEC_TAG_PatternWidth:
        case CODEC_TAG_PatternHeight:
        case CODEC_TAG_ComponentsPerSample:
        case CODEC_TAG_MaxBitsPerComponent:
#endif
            return true;
            
        default:
            return false;
    }
}

/*!
	@brief Return the header mask that corresponds to the header tag
 */
uint16_t GetHeaderMask(TAGWORD tag)
{
    uint16_t header_mask = 0;
    
    switch (tag)
    {
        case CODEC_TAG_ImageWidth:
            header_mask = BITSTREAM_HEADER_FLAGS_IMAGE_WIDTH;
            break;
            
        case CODEC_TAG_ImageHeight:
            header_mask = BITSTREAM_HEADER_FLAGS_IMAGE_HEIGHT;
            break;
            
        case CODEC_TAG_ChannelCount:
            header_mask = BITSTREAM_HEADER_FLAGS_CHANNEL_COUNT;
            break;
            
        case CODEC_TAG_SubbandCount:
            header_mask = BITSTREAM_HEADER_FLAGS_SUBBAND_COUNT;
            break;
            
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_ImageFormat:
            header_mask = BITSTREAM_HEADER_FLAGS_IMAGE_FORMAT;
            break;
#endif
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_PatternWidth:
            header_mask = BITSTREAM_HEADER_FLAGS_PATTERN_WIDTH;
            break;
#endif
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_PatternHeight:
            header_mask = BITSTREAM_HEADER_FLAGS_PATTERN_HEIGHT;
            break;
#endif
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_ComponentsPerSample:
            header_mask = BITSTREAM_HEADER_FLAGS_COMPONENTS_PER_SAMPLE;
            break;
#endif
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
        case CODEC_TAG_MaxBitsPerComponent:
            header_mask = BITSTREAM_HEADER_FLAGS_MAX_BITS_PER_COMPONENT;
            break;
#endif
            
        default:
            assert(0);
            break;
    }
    
    return header_mask;
}

/*!
	@brief Record that a header parameter was found in the bitstream.
	The tag-value pair that corresponds to a header parameters must occur
	in the bitstream header and must occur at most once in the bitstream.
 */
CODEC_ERROR UpdateHeaderParameter(DECODER *decoder, TAGWORD tag)
{
    uint16_t header_mask = 0;
    
    if (!IsHeaderParameter(tag)) {
        return CODEC_ERROR_UNEXPECTED;
    }
    
    header_mask = GetHeaderMask(tag);
    
    if (header_mask == 0) {
        return CODEC_ERROR_UNEXPECTED;
    }
    
    if (decoder->header_mask & header_mask) {
        // The header parameter should occur at most once
        return CODEC_ERROR_DUPLICATE_HEADER_PARAMETER;
    }
    
    // Record this encounter with the header parameter
    decoder->header_mask |= header_mask;
    
    return CODEC_ERROR_OKAY;
}

#if VC5_ENABLED_PART(VC5_PART_COLOR_SAMPLING)
/*!
	@brief Adjust the width of the layer (if necessary)
	Note that all layers have the same dimensions so the layer index is not
	passed as an argument to this routine.
	All layers have the same width as the encoded width.
 */
DIMENSION LayerWidth(DECODER *decoder, DIMENSION width)
{
    //CODEC_STATE *codec = &decoder->codec;
    (void)decoder;
    return width;
}
#endif

#if VC5_ENABLED_PART(VC5_PART_COLOR_SAMPLING)
/*!
	@brief Adjust the height of the layer to account for interlaced frames
	Note that all layers have the same dimensions so the layer index is not
	passed as an argument to this routine.
 */
DIMENSION LayerHeight(DECODER *decoder, DIMENSION height)
{
    CODEC_STATE *codec = &decoder->codec;
    
    if (codec->progressive == 0)
    {
        height /= 2;
    }
    
    return height;
}
#endif

/*!
	@brief Decode the specified wavelet subband
	After decoded the specified subband, the routine checks whether all bands
	in the current wavelet have been decoded and if so the inverse transform is
	applied to the wavelet to reconstruct the lowpass band in the wavelet at the
	next lower level.
 */
CODEC_ERROR DecodeChannelSubband(DECODER *decoder, BITSTREAM *input, size_t chunk_size)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    CODEC_STATE *codec = &decoder->codec;
    
    const int channel_number = codec->channel_number;
    const int subband_number = codec->subband_number;
    
    // Get the index to the wavelet corresponding to this subband
    const int index = SubbandWaveletIndex(subband_number);
    
    // Get the index of the wavelet band corresponding to this subband
    const int band = SubbandBandIndex(subband_number);
    
    // Wavelet containing the band to decode
    WAVELET *wavelet = NULL;
    
    //TODO: Need to check that the codeblock matches the chunk size
    (void)chunk_size;
    
    // Allocate the wavelets for this channel if not already allocated
    AllocateChannelWavelets(decoder, channel_number);
    
    // Is this a highpass band?
    if (subband_number > 0)
    {
        // Decode a highpass band
        
        // Get the wavelet that contains the highpass band
        wavelet = decoder->transform[channel_number].wavelet[index];
        
        // The wavelets are preallocated
        assert(wavelet != NULL);
        
        error = DecodeHighpassBand(decoder, input, wavelet, band);
        if (error == CODEC_ERROR_OKAY)
        {
            // Update the wavelet band valid flags
            UpdateWaveletValidBandMask(wavelet, band);
        }
        
        // Save the quantization factor
        wavelet->quant[band] = codec->band.quantization;
    }
    else
    {
        // Decode a lowpass band
        
        // Get the wavelet that contains the lowpass band
        wavelet = decoder->transform[channel_number].wavelet[index];
        
        // The lowpass band must be subband zero
        assert(subband_number == 0);
        
        // The lowpass data is always stored in wavelet band zero
        assert(band == 0);
        
        // The wavelets are preallocated
        assert(wavelet != NULL);
        
        error = DecodeLowpassBand(decoder, input, wavelet);
        if (error == CODEC_ERROR_OKAY)
        {
            // Update the wavelet band valid flags
            UpdateWaveletValidBandMask(wavelet, band);
        }
    }
    
    // Ready to invert this wavelet to get the lowpass band in the lower wavelet?
    if (BandsAllValid(wavelet))
    {
        // Apply the inverse wavelet transform to reconstruct the lower level wavelet
        error = ReconstructWaveletBand(decoder, channel_number, wavelet, index);
    }
    
    return error;
}

/*!
	@brief Invert the wavelet to reconstruct a lowpass band
	The bands in the wavelet at one level are used to compute the lowpass
	band in the wavelet at the next lower level in the transform.  Wavelet
	levels are numbered starting at zero for the original image.  The
	reference codec for the baseline profile uses the classic wavelet
	tree where each wavelet at a high level depends only on the wavelet
	at the next lower level and each wavelet is a spatial wavelet with
	four bands.
	This routine is called during decoding after all bands in a wavelet
	have been decoded and the lowpass band in the wavelet at the next
	lower level can be computed by applying the inverse wavelet transform.
	This routine is not called for the wavelet at level one to reconstruct the
	decoded component arrays.  Special routines are used to compute each component
	array using the wavelet at level one in each channel.
	
	See @ref ReconstructUnpackedImage.
 */
CODEC_ERROR ReconstructWaveletBand(DECODER *decoder, int channel, WAVELET *wavelet, int index)
{
    PRESCALE prescale = decoder->codec.prescale_table[index];
    
    // Is the current wavelet at a higher level than wavelet level one?
    if (index > 0)
    {
        // Reconstruct the lowpass band in the lower wavelet
        const int lowpass_index = index - 1;
        WAVELET *lowpass;
        int lowpass_width;
        int lowpass_height;
        
        lowpass = decoder->transform[channel].wavelet[lowpass_index];
        assert(lowpass != NULL);
        if (! (lowpass != NULL)) {
            return CODEC_ERROR_UNEXPECTED;
        }
        
        lowpass_width = lowpass->width;
        lowpass_height = lowpass->height;
        
        // Check that the reconstructed wavelet is valid
        if( lowpass_width <= 0 || lowpass_height <= 0 )
        {
            assert(false);
            return CODEC_ERROR_IMAGE_DIMENSIONS;
        }
        
        // Check that the lowpass band has not already been reconstructed
        assert((lowpass->valid_band_mask & BandValidMask(0)) == 0);
        
        // Check that all of the wavelet bands have been decoded
        assert(BandsAllValid(wavelet));
        
        // Decode the lowpass band in the wavelet one lower level than the input wavelet
        TransformInverseSpatialQuantLowpass(decoder->allocator, wavelet, lowpass, prescale);
        
        // Update the band valid flags
        UpdateWaveletValidBandMask(lowpass, 0);
    }
    
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Set the bit for the specified subband in the decoded band mask
	The decoded subband mask is used to track which subbands have been
	decoded in the current channel.  It is reset at the start of each
	channel.
 */
CODEC_ERROR SetDecodedBandMask(CODEC_STATE *codec, int subband)
{
    if (0 <= subband && subband < MAX_SUBBAND_COUNT) {
        codec->decoded_subband_mask |= (1 << subband);
    }
    return CODEC_ERROR_OKAY;
}

/*!
	@brief Decoded the lowpass band from the bitstream
	The wavelet at the highest level is passes as an argument.
	This routine decodes lowpass band in the bitstream into the
	lowpass band of the wavelet.
 */
CODEC_ERROR DecodeLowpassBand(DECODER *decoder, BITSTREAM *stream, WAVELET *wavelet)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    CODEC_STATE *codec = &decoder->codec;
    
    int lowpass_band_width;			// Lowpass band dimensions
    int lowpass_band_height;
    int lowpass_band_pitch;
    PIXEL *lowpass_band_ptr;		// Pointer into the lowpass band
    
    PRECISION lowpass_precision;	// Number of bits per lowpass coefficient
    
    int row, column;
    
    lowpass_band_width = wavelet->width;
    lowpass_band_height = wavelet->height;
    lowpass_band_pitch = wavelet->pitch/sizeof(PIXEL);
    lowpass_band_ptr = wavelet->data[0];
    
    lowpass_precision = codec->lowpass_precision;
    
    // Decode each row in the lowpass image
    for (row = 0; row < lowpass_band_height; row++)
    {
        for (column = 0; column < lowpass_band_width; column++)
        {
            COEFFICIENT lowpass_value = (COEFFICIENT)GetBits(stream, lowpass_precision);
            //assert(0 <= lowpass_value && lowpass_value <= COEFFICIENT_MAX);
            
            //if (lowpass_value > COEFFICIENT_MAX) {
            //	lowpass_value = COEFFICIENT_MAX;
            //}
            
            lowpass_band_ptr[column] = lowpass_value;
        }
        
        // Advance to the next row in the lowpass image
        lowpass_band_ptr += lowpass_band_pitch;
    }
    // Align the bitstream to the next tag value pair
    AlignBitsSegment(stream);
    
    // Return indication of lowpass decoding success
    return error;
}

/*!
	@brief Decode the highpass band from the bitstream
	The specified wavelet band is decoded from the bitstream
	using the codebook and encoding method specified in the
	bitstream.
 */
CODEC_ERROR DecodeHighpassBand(DECODER *decoder, BITSTREAM *stream, WAVELET *wavelet, int band)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Get the highpass band dimensions
    DIMENSION width = wavelet->width;	//codec->band.width;
    DIMENSION height = wavelet->height;	//codec->band.height;
    
    // Check that the band index is in range
    assert(0 <= band && band < wavelet->band_count);
    
    // Encoded coefficients start on a tag boundary
    AlignBitsSegment(stream);
    
    // Decode this subband
    error = DecodeBandRuns(stream, decoder->codebook, wavelet->data[band], width, height, wavelet->pitch);
    assert(error == CODEC_ERROR_OKAY);
    
    // Return failure if a problem was encountered while reading the band coefficients
    if (error != CODEC_ERROR_OKAY) {
        return error;
    }
    
    // The encoded band coefficients end on a bitstream word boundary
    // to avoid interference with the marker for the coefficient band trailer
    AlignBitsWord(stream);
    
    // Decode the band trailer
    error = DecodeBandTrailer(stream);
    decoder->error = error;
    assert(error == CODEC_ERROR_OKAY);
    return error;
}

/*!
	@brief Decode the highpass band from the bitstream
	The highpass band in the bitstream is decoded using the specified
	codebook.  This routine assumes that the highpass band was encoded
	using the run lengths encoding method which is the default for all
	current codec implementations.
	The encoded highpass band consists of signed values and runs of zeros.
	Each codebook entry specifies either an unsigned magnitude with a run
	length of one or a run of zeros.  The unsigned magnitude is immediately
	followed by the sign bit.
	Unsigned magnitudes always have a run length of one.
	Note that runs of zeros can straddle end of line boundaries.
	The end of the highpass band is marked by a special codeword.
	Special codewords in the codebook have a run length of zero.
	The value indicates the type or purpose of the special codeword.
 */
CODEC_ERROR DecodeBandRuns(BITSTREAM *stream, CODEBOOK *codebook, PIXEL *data,
                           DIMENSION width, DIMENSION height, DIMENSION pitch)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    size_t data_count;
    size_t row_padding;
    int row = 0;
    int column = 0;
    int index = 0;
    //BITWORD special;
    RUN run = RUN_INITIALIZER;
    
    // Convert the pitch to units of pixels
    pitch /= sizeof(PIXEL);
    
    // Check that the band dimensions are reasonable
    assert(width <= pitch);
    
    // Compute the number of pixels encoded into the band
    data_count = height * width;
    row_padding = pitch - width;
    
    while (data_count > 0)
    {
        // Get the next run length and value
        error = GetRun(stream, codebook, &run);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        // Check that the run does not extend past the end of the band
        assert(run.count <= data_count);
        
        // Copy the value into the specified number of pixels in the band
        while (run.count > 0)
        {
            // Reached the end of the column?
            if (column == width)
            {
                // Need to pad the end of the row?
                if (row_padding > 0)
                {
                    int count;
                    for (count = 0; (size_t)count < row_padding; count++) {
                        data[index++] = 0;
                    }
                }
                
                // Advance to the next row
                row++;
                column = 0;
            }
            
            data[index++] = (PIXEL)run.value;
            column++;
            run.count--;
            data_count--;
        }
    }
    
    // The last run should have ended at the end of the band
    assert(data_count == 0 && run.count == 0);
    
    // Check for the special codeword that marks the end of the highpass band
    error = GetRlv(stream, codebook, &run);
    if (error == CODEC_ERROR_OKAY) {
        if (! (run.count == 0 || run.value == SPECIAL_MARKER_BAND_END)) {
            error = CODEC_ERROR_BAND_END_MARKER;
        }
    }
    
    return error;
}

/*!
	@brief Decode the band trailer that follows a highpass band
	This routine aligns the bitstream to a tag value boundary.
	Currently the band trailer does not perform any function beyond
	preparing the bitstream for reading the next tag value pair.
 */
CODEC_ERROR DecodeBandTrailer(BITSTREAM *stream)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    // Advance the bitstream to a tag boundary
    AlignBitsSegment(stream);
    
    return error;
}

/*!
	@brief Return true if the bitstream has been completely decoded
	The end of sample flag is set to true when enough of the sample has been read
	from the bitstream to allow the output frame to be fully reconstructed.  Any
	remaining bits in the sample can be ignored and it may be the case that further
	reads from the bitstream will result in an error.
	The end of sample flag is set when the tag for the frame trailer is found, but
	may be set when sufficient subbands have been decoded to allow the frame to be
	reconstructed at the desired resolution.  For example, it is not an error if
	bands at level one in the wavelet tree are not present in the bitstream when
	decoding to half resolution.  The decoder should set the end of sample flag as
	soon as it is no longer necessary to read further information from the sample.
	
	@todo Rename this routine to end of image or end of bitstream?
 */
bool EndOfSample(DECODER *decoder)
{
    return decoder->codec.end_of_sample;
}

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Return true of the layer has been completely read from the bitstream
 */
bool EndOfLayer(DECODER *decoder)
{
    return (decoder->codec.end_of_layer || decoder->codec.end_of_sample);
}
#endif

/*!
	@brief Return true if the entire bitstream header has been decoded
	The bitstream header has been completely decoded when at least one
	non-header parameter has been encountered in the bitstream and all
	of the required header parameters have been decoded.
	
	@todo Create a bitstream that can be used to test this predicate.
 */
bool IsHeaderComplete(DECODER *decoder)
{
    return (decoder->header_finished &&
            ((decoder->header_mask & BITSTREAM_HEADER_FLAGS_REQUIRED) == BITSTREAM_HEADER_FLAGS_REQUIRED));
}

/*!
	@brief Return true if all channels in the bitstream have been processed
	It is only necessary to test the bands in the largest wavelet in each
	channel since its lowpass band would not be finished if the wavelets
	at the higher levels were incomplete.
 */
bool IsDecodingComplete(DECODER *decoder)
{
    int channel_count = decoder->codec.channel_count;
    int channel_index;
    
    for (channel_index = 0; channel_index < channel_count; channel_index++)
    {
        WAVELET *wavelet = decoder->transform[channel_index].wavelet[0];
        
        // Processing is not complete if the wavelet has not been allocated
        if (wavelet == NULL) return false;
        
        // Processing is not complete unless all bands have been processed
        if (!AllBandsValid(wavelet)) return false;
    }
    
    // All bands in all wavelets in all channels are done
    return true;
}

/*!
	@brief Perform the final wavelet transform in each channel to compute the component arrays
	Each channel is decoded and the lowpass and highpass bands are used to reconstruct the
	lowpass band in the wavelet at the next lower level by applying the inverse wavelet filter.
	Highpass band decoding and computation of the inverse wavelet transform in each channel
	stops when the wavelet at the level immediately above the output frame is computed.
	This routine performs the final wavelet transform in each channel and combines the channels
	into a single output frame.  Note that this routine is called for each layer in a sample,
	producing an output frame for each layer.  The output frames for each layer must be combine
	by an image compositing operation into a single output frame for the fully decoded sample.
 */
CODEC_ERROR ReconstructUnpackedImage(DECODER *decoder, UNPACKED_IMAGE *image)
{
    TIMESTAMP("[BEG]", 2)
    
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    gpr_allocator *allocator = decoder->allocator;
    
    int channel_count = decoder->codec.channel_count;
    int channel_number;
    
    // Check for enough space in the local array allocations
    //assert(channel_count <= MAX_CHANNEL_COUNT);
    
    // Allocate the vector of component arrays
    size_t size = channel_count * sizeof(COMPONENT_ARRAY);
    image->component_array_list = allocator->Alloc(size);
    if (image->component_array_list == NULL) {
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    // Clear the component array information so that the state is consistent
    image->component_count = 0;
    memset(image->component_array_list, 0, size);
    
    for (channel_number = 0; channel_number < channel_count; channel_number++)
    {
        // Get the dimensions of this channel
        DIMENSION channel_width = decoder->channel[channel_number].width;
        DIMENSION channel_height = decoder->channel[channel_number].height;
        PRECISION bits_per_component = decoder->channel[channel_number].bits_per_component;
        
        // Amount of prescaling applied to the component array values before encoding
        PRESCALE prescale = decoder->codec.prescale_table[0];
        
        // Allocate the component array for this channel
        error = AllocateComponentArray(allocator,
                                       &image->component_array_list[channel_number],
                                       channel_width,
                                       channel_height,
                                       bits_per_component);
        
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
        
        error = TransformInverseSpatialQuantArray(allocator,
                                                  decoder->transform[channel_number].wavelet[0],
                                                  image->component_array_list[channel_number].data,
                                                  channel_width,
                                                  channel_height,
                                                  image->component_array_list[channel_number].pitch,
                                                  prescale);
        if (error != CODEC_ERROR_OKAY) {
            return error;
        }
    }
    
    // One component array is output by the decoding process per channel in the bitstream
    image->component_count = channel_count;
    
    TIMESTAMP("[END]", 2)
    
    return error;
}

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Perform the final wavelet transform in each channel to compute the output frame
	Each channel is decoded and the lowpass and highpass bands are used to reconstruct the
	lowpass band in the wavelet at the next lower level by applying the inverse wavelet filter.
	Highpass band decoding and computation of the inverse wavelet transform in each channel
	stops when the wavelet at the level immediately above the output frame is computed.
	This routine performs the final wavelet transform in each channel and combines the channels
	into a single output frame.  Note that this routine is called for each layer in a sample,
	producing an output frame for each layer.  The output frames for each layer must be combine
	by an image compositing operation into a single output frame for the fully decoded sample.
	Refer to @ref ReconstructSampleFrame for the details of how the frames from each layer
	are combined to produce the output frame for the decoded sample.
 */
CODEC_ERROR ReconstructLayerImage(DECODER *decoder, IMAGE *image)
{
    CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    DIMENSION decoded_width = decoder->decoded.width;
    DIMENSION decoded_height = decoder->decoded.height;
    int channel_count = decoder->codec.channel_count;
    
    //DIMENSION layer_width = LayerWidth(decoder, decoded_width);
    //DIMENSION layer_height = LayerHeight(decoder, decoded_height);
    DIMENSION layer_width = decoded_width;
    DIMENSION layer_height = decoded_height;
    
    //TODO: Adjust the layer width to account for chroma sampling
    
    // Allocate a buffer for the intermediate output from each wavelet transform
    size_t decoded_frame_pitch = layer_width * channel_count * sizeof(PIXEL);
    size_t decoded_frame_size = layer_height * decoded_frame_pitch;
    PIXEL *decoded_frame_buffer = (PIXEL *)Alloc(decoder->allocator, decoded_frame_size);
    if (decoded_frame_buffer == NULL) {
        return CODEC_ERROR_OUTOFMEMORY;
    }
    
    error = TransformInverseSpatialQuantBuffer(decoder, decoded_frame_buffer, (DIMENSION)decoded_frame_pitch);
    if (error == CODEC_ERROR_OKAY)
    {
        // Pack the decoded frame into the output format
        error = PackOutputImage(decoded_frame_buffer, decoded_frame_pitch, decoder->encoded.format, image);
    }
    
    // Free the buffer for the decoded frame
    Free(decoder->allocator, decoded_frame_buffer);
    
    return error;
}
#endif

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
/*!
	@brief Combine multiple decoded frames from each layer into a single output frame
	An encoded sample may contain multiple sub-samples called layers.  For example,
	there may be two sub-samples (layers) for the left and right frames in a stereo pair.
	Note that the baseline profile supports only one layer per sample.
	Each layer is decoded independently to produce an output frame for that layer.  The
	CineForm codec does not support dependent sub-samples in any of the existing profiles.
	
	This routine forms a composite frame for the output of the completely decoded sample
	from the individual frames obtained by decoding each layer.  It is contemplated that
	any image compositing algorithm could be used to combine decoded layers, although the
	most sophisticated algorithms might be reserved for the most advanced profiles.
	The dimensions of the output frame could be much larger than the dimensions of any
	of the frames decoded from individual layers.  Compositing could overlay the frames
	from the individual layers with an arbitrary spatial offset applied to the frame from
	each layer, creating a collage from frames decoded from the individual layers.  Typical
	applications may use only the most elementary compositing operations.
 */
CODEC_ERROR ReconstructSampleFrame(DECODER *decoder, IMAGE image_array[], int frame_count, IMAGE *output_image)
{
    DIMENSION frame_width = image_array[0].width;
    DIMENSION field_height = image_array[0].height;
    DIMENSION frame_height = 2 * field_height;
    PIXEL_FORMAT frame_format = image_array[0].format;
    
    AllocImage(decoder->allocator, output_image, frame_width, frame_height, frame_format);
    
    return ComposeFields(image_array, frame_count, output_image);
}
#endif


