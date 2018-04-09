/*! @file decoder.h
 *
 *  @brief Core decoder functions and data structure
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

#ifndef DECODER_H
#define DECODER_H

/*!
	Data structure for the buffers and information used by
	the decoder.
 
	The decoder data structure contains information that will be
	used by the decoder for decoding every sample in the sequence.
	Information that varies during decoding, such as the current
	subband index or the dimensions of the bands in the wavelet that
	is being decoded, is stored in the codec state.
 
	@todo Consider changing the transform data structure to use a
	vector of wavelets rather than a vector of wavelet pointers.
	
	@todo Remove unused substructures
 
	@todo Dynamically allocate the vector of wavelet trees based on the
	actual number of channels rather than the maximum channel count.
	
	@todo Need to handle the cases where header parameters are provided
	by the application instead of being in the bitstream.
 */
typedef struct _decoder
{
    CODEC_ERROR error;			//!< Error code from the most recent codec operation
    gpr_allocator *allocator;		//!< Memory allocator used to allocate all dyynamic data
    CODEC_STATE codec;			//!< Information gathered while decoding the current sample
    
    //! Parts of the VC-5 standard that are supported at runtime by the codec implementation
    ENABLED_PARTS enabled_parts;
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    uint64_t frame_number;		//!< Every sample in a clip has a unique frame number
#endif
    
    uint16_t header_mask;		//!< Track which header parameters have been decoded
    bool header_finished;		//!< Finished decoding the bitstream header?
    bool memory_allocated;		//!< True if memory for decoding has been allocated
    
    //! Dimensions of each channel found in the bitstream
    struct _channel
    {
        DIMENSION width;				//!< Width of this channel
        DIMENSION height;				//!< Height of this channnel
        
        //! Bits per component for the component array corresponding to this channel
        uint_least8_t bits_per_component;
        
        bool initialized;				//!< Has the channel information been initialized?
        
        bool found_first_codeblock;     //!< Has the first codeblock in the channel been found?
        
    } channel[MAX_CHANNEL_COUNT];	//!< Information about each channel in the bitstream
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    //! Dimensions and format of the encoded image
    struct _encoded
    {
        DIMENSION width;		//!< Encoded width
        DIMENSION height;		//!< Encoded height
        IMAGE_FORMAT format;	//!< Encoded format
        
    } encoded;			//!< Information about the image as represented in the bitstream
#endif
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    //! Dimensions and format of the decoded image
    struct _decoded
    {
        DIMENSION width;		//!< Decoded width
        DIMENSION height;		//!< Decoded height
        //RESOLUTION resolution;
        
    } decoded;			//!< Information about the decoded component arrays
#endif
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    //! Dimensions and format of the frame after post-processing (see @ref ImageRepackingProcess)
    struct _output
    {
        DIMENSION width;		//!< Output frame width
        DIMENSION height;		//!< Output frame height
        PIXEL_FORMAT format;	//!< Output frame pixel format
        
    } output;			//!< Information about the packed image output by the image repacking process
#endif
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    //! Dimensions and format of the image output by the display process
    struct _display
    {
        DIMENSION width;		//!< Output frame width
        DIMENSION height;		//!< Output frame height
        PIXEL_FORMAT format;	//!< Output frame pixel format
        
    } display;			//!< Information about the displayable image output by the display process
#endif
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    int layer_count;			//!< Number of subsamples in each sample
#endif
    
    int wavelet_count;			//!< Number of wavelets in each channel
    
    int subbands_to_decode;
    
    //! Wavelet tree for each channel
    TRANSFORM transform[MAX_CHANNEL_COUNT];
    
    //! Pointer to the active codebook for variable-length codes
    CODEBOOK *codebook;
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    uint8_t image_sequence_identifier[16];      //!< UUID for the unique image sequence identifier
    uint32_t image_sequence_number;             //!< Number of the image in the image sequence
#endif
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    bool progressive;			//!< True if the encoded frame is progressive
    bool top_field_first;		//!< True if the top field is encoded first
#endif
    
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    bool section_flag;          //!< Control whether section processing is enabled
    FILE *section_logfile;      //!< Log file for writing section information
#endif
    
} DECODER;

/*!
	@brief Information that can be obtained from an bitstream header
 
	The bitstream header consists of tag-value pairs that must occur in the bitstream
	before the first codeblock if the parameters are present in the bitstream.
 
	Consider organizing the values obtained from the bitstream into input parameters
	and encoded parameters, as is done elsewhere in the decoder, even though the
	bitstream does not have such a rigid syntax.
 */
typedef struct _bitstream_header
{
    uint16_t channel_count;			//!< Number of channels in the bitstream
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    uint64_t frame_number;			//!< Every sample in a clip has a unique frame number
    PIXEL_FORMAT input_format;		//!< Pixel format of the frame input to the encoder
    
    // Encoded dimensions and format of the encoded frame (including padding)
    DIMENSION encoded_width;			//!< Width of the encoded frame
    DIMENSION encoded_height;			//!< Height of the encoded frame
    
    IMAGE_FORMAT encoded_format;		//!< Encoded format
    
    // The display aperture within the encoded frame
    DIMENSION row_offset;
    DIMENSION column_offset;
    DIMENSION display_width;			//!< Width of the displayable frame (if specified in the sample)
    DIMENSION display_height;			//!< Height of the displayable frame (if specified in the sample)
#endif
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    DIMENSION video_channel_count;		// Number of layers?
    DIMENSION current_video_channel;	//TODO: Find better way to handle this
    int layer_count;					//!< Number of layers in the sample
    bool progressive;					//!< Progressive versus interlaced frames
    bool top_field_first;				//!< Interlaced frame with top field first
#endif
    
} BITSTREAM_HEADER;

//! Flags that indicate which header parameters have been assigned values
typedef enum _bitstream_header_flags
{
    BITSTREAM_HEADER_FLAGS_IMAGE_WIDTH = (1 << 0),
    BITSTREAM_HEADER_FLAGS_IMAGE_HEIGHT = (1 << 1),
    BITSTREAM_HEADER_FLAGS_CHANNEL_COUNT = (1 << 2),
    BITSTREAM_HEADER_FLAGS_SUBBAND_COUNT = (1 << 3),
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    BITSTREAM_HEADER_FLAGS_IMAGE_FORMAT = (1 << 4),
    BITSTREAM_HEADER_FLAGS_PATTERN_WIDTH = (1 << 5),
    BITSTREAM_HEADER_FLAGS_PATTERN_HEIGHT = (1 << 6),
    BITSTREAM_HEADER_FLAGS_COMPONENTS_PER_SAMPLE = (1 << 7),
    BITSTREAM_HEADER_FLAGS_MAX_BITS_PER_COMPONENT = (1 << 8),
    
    //! Required header parameters
    BITSTREAM_HEADER_FLAGS_REQUIRED = (BITSTREAM_HEADER_FLAGS_IMAGE_WIDTH |
                                       BITSTREAM_HEADER_FLAGS_IMAGE_HEIGHT |
                                       BITSTREAM_HEADER_FLAGS_IMAGE_FORMAT |
                                       BITSTREAM_HEADER_FLAGS_PATTERN_WIDTH |
                                       BITSTREAM_HEADER_FLAGS_PATTERN_HEIGHT |
                                       BITSTREAM_HEADER_FLAGS_COMPONENTS_PER_SAMPLE),
#else
    
    //! Required header parameters
    BITSTREAM_HEADER_FLAGS_REQUIRED = (BITSTREAM_HEADER_FLAGS_IMAGE_WIDTH |
                                       BITSTREAM_HEADER_FLAGS_IMAGE_HEIGHT),
#endif
    
} BITSTREAM_HEADER_FLAGS;

#ifdef __cplusplus
extern "C" {
#endif
    
    CODEC_ERROR InitDecoder(DECODER *decoder, const gpr_allocator *allocator);
    
    CODEC_ERROR SetDecoderLogfile(DECODER *decoder, FILE *logfile);
    
    CODEC_ERROR ReleaseDecoder(DECODER *decoder);
    
    CODEC_ERROR PrepareDecoderState(DECODER *decoder, const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR PrepareDecoderTransforms(DECODER *decoder);
    
    CODEC_ERROR SetOutputImageFormat(DECODER *decoder,
                                     const DECODER_PARAMETERS *parameters,
                                     DIMENSION *width_out,
                                     DIMENSION *height_out,
                                     PIXEL_FORMAT *format_out);
    
#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    CODEC_ERROR SetDisplayImageFormat(DECODER *decoder,
                                      const DECODER_PARAMETERS *parameters,
                                      DIMENSION *width_out,
                                      DIMENSION *height_out,
                                      PIXEL_FORMAT *format_out);
#endif
    
    bool ChannelLowpassBandsAllValid(const DECODER *decoder, int wavelet_index);
    
    PIXEL_FORMAT EncodedPixelFormat(const DECODER *decoder, const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR PackOutputImage(void *buffer, size_t pitch, int encoded_format, IMAGE *image);
    
    CODEC_ERROR ImageRepackingProcess(const UNPACKED_IMAGE *unpacked_image,
                                      PACKED_IMAGE *packed_image,
                                      const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR UpdateCodecState(DECODER *decoder, BITSTREAM *stream, TAGVALUE segment);
    
    bool IsHeaderParameter(TAGWORD tag);
    
    CODEC_ERROR UpdateHeaderParameter(DECODER *decoder, TAGWORD tag);
    
    CODEC_ERROR PrepareDecoder(DECODER *decoder, const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR AllocDecoderTransforms(DECODER *decoder);
    
    CODEC_ERROR ReleaseDecoderTransforms(DECODER *decoder);
    
    CODEC_ERROR AllocDecoderBuffers(DECODER *decoder);
    
    CODEC_ERROR ReleaseDecoderBuffers(DECODER *decoder);
    
    CODEC_ERROR AllocateChannelWavelets(DECODER *decoder, int channel);
    
    CODEC_ERROR DecodeStream(STREAM *stream, UNPACKED_IMAGE *image, const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR DecodeImage(STREAM *stream, IMAGE *image, RGB_IMAGE *rgb_image, DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR DecodingProcess(DECODER *decoder, BITSTREAM *stream, UNPACKED_IMAGE *image, const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR DecodeSingleImage(DECODER *decoder, BITSTREAM *input, UNPACKED_IMAGE *image, const DECODER_PARAMETERS *parameters);
    
    CODEC_ERROR DecodeSampleLayer(DECODER *decoder, BITSTREAM *input, IMAGE *image);
    
    CODEC_ERROR DecodeChannelSubband(DECODER *decoder, BITSTREAM *input, size_t chunk_size);
    
    CODEC_ERROR ReconstructWaveletBand(DECODER *decoder, int channel, WAVELET *wavelet, int index);
    
    CODEC_ERROR ParseChannelIndex(BITSTREAM *stream, uint32_t *channel_size, int channel_count);
    
    DIMENSION LayerWidth(DECODER *decoder, DIMENSION width);
    DIMENSION LayerHeight(DECODER *decoder, DIMENSION height);
    
    CODEC_ERROR ProcessSampleMarker(DECODER *decoder, BITSTREAM *stream, TAGWORD marker);
    
    CODEC_ERROR SetDecodedBandMask(CODEC_STATE *codec, int subband);
    
    CODEC_ERROR DecodeLowpassBand(DECODER *decoder, BITSTREAM *stream, WAVELET *wavelet);
    
    CODEC_ERROR DecodeHighpassBand(DECODER *decoder, BITSTREAM *stream, WAVELET *wavelet, int band);
    
    CODEC_ERROR DecodeBandRuns(BITSTREAM *stream, CODEBOOK *codebook, PIXEL *data,
                               DIMENSION width, DIMENSION height, DIMENSION pitch);
    
    CODEC_ERROR DecodeBandTrailer(BITSTREAM *stream);
    
    CODEC_ERROR DecodeSampleChannelHeader(DECODER *decoder, BITSTREAM *stream);
    
    bool IsHeaderComplete(DECODER *decoder);
    
    bool EndOfSample(DECODER *decoder);
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    bool EndOfLayer(DECODER *decoder);
    bool IsLayerComplete(DECODER *decoder);
#endif
    
    bool IsDecodingComplete(DECODER *decoder);
    
    CODEC_ERROR ReconstructUnpackedImage(DECODER *decoder, UNPACKED_IMAGE *image);
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    CODEC_ERROR ReconstructLayerImage(DECODER *decoder, IMAGE *image);
#endif
    
    CODEC_ERROR TransformInverseSpatialQuantBuffer(DECODER *decoder, void *output_buffer, DIMENSION output_width, DIMENSION output_pitch);
    
#ifdef __cplusplus
}
#endif

#endif // DECODER_H
