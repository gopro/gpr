/*! @file encoder.h
 *
 *  @brief Declaration of the data structures and constants used for core encoding.
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

#ifndef ENCODER_H
#define ENCODER_H

/*!
	@brief Data structure for the pixel or picture aspect ratio

	@todo Should the members of the aspect ratio data structure be unsigned?
*/
typedef struct _aspect_ratio
{
	int16_t x;		//!< Numerator of the aspect ratio
	int16_t y;		//!< Denominator of the aspect ratio

} ASPECT_RATIO;

/*!
	@brief Data structure for the buffers and information used by the encoder

	The encoder data structure contains information that will be
	used by the encoder for decoding every sample in the sequence.
	Information that varies during decoding, such as the current
	subband index or the dimensions of the bands in the wavelet that
	is being decoded, is stored in the codec state.

	The encoded dimensions are the width and height of the array of pixels
	for each encoded channel (image plane), including padding added to
	satisfy the requirements of the wavelet transforms.  In the case
	of 4:2:2 sampling, the encoded width and height are for the luma channel.

	The display dimensions are the width and height of the display aperture,
	the displayable portion of the decoded image with padding removed.
	
	The display dimensions can include a row and column offset to trim
	top rows and left columns from the decoded image prior to display.

	The decoded dimensions equal the encoded dimensions at full resolution
	and are reduced by a power of two if decoded to a lower resolution.
	The decoded dimensions are derived from the encoded dimensions and the
	decoded resolution.

	The decoded dimensions are used to allocate the wavelet tree for the
	lowpass and highpass coefficients decoded from the bitstream.  It is
	not necessary to allocate wavelets for larger resolutions than the
	decoded resolution.

	For Bayer encoded images, the encoded dimensions are half the width
	and height of the input dimensions (after windowing).  Typically,
	media containers report the display dimensions as twice the encoded
	dimensions since a demosaic algorithm must be applied to produce a
	displayable image that looks right to most people.

	@todo Consider changing the transform data structure to use a
	vector of wavelets rather than a vector of wavelet pointers.
*/
typedef struct _encoder
{
	//	CODEC codec;			//!< Common fields for both the encoder and decoder

	FILE *logfile;				//!< File for writing debugging information
	CODEC_ERROR error;			//!< Error code from the most recent codec operation
	gpr_allocator *allocator;		//!< Memory allocator used to allocate all dyynamic data
	CODEC_STATE codec;			//!< Information gathered while decoding the current sample
	VERSION version;			//!< Codec version (major, minor, revision, build)

	//! Parts of the VC-5 standard that are supported at runtime by the codec implementation
	ENABLED_PARTS enabled_parts;

	uint64_t frame_number;		//!< Every sample in a clip has a unique frame number

	//! Number of color channels in the input and encoded images
	uint_fast8_t channel_count;

	//! Number of wavelet transforms in each channel
	uint_fast8_t wavelet_count;

	//! Internal precision used by this encoder
	PRECISION internal_precision;

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
	IMAGE_FORMAT image_format;			//!< Type of the image represented by the bitstream
	DIMENSION image_width;				//!< Number of samples per row in the image represented by the bitstream
	DIMENSION image_height;				//!< Number of rows of samples in the image represented by the bitstream
	DIMENSION pattern_width;			//!< Number of samples per row in each pattern element
	DIMENSION pattern_height;			//!< Number of rows of samples in each pattern element
	DIMENSION components_per_sample;	//!< Number of components per sample in the image
	DIMENSION max_bits_per_component;	//!< Upper bound on the number of significant bits per component value
#else
	DIMENSION image_width;				//!< Upper bound on the width of each channel
	DIMENSION image_height;				//!< Upper bound on the height of each channel
#endif

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	//! Progressive frame flag
	BOOLEAN progressive;

	// Interlaced frame with the top field encoded first
	BOOLEAN top_field_first;

	// The encoded frame is upside down (not used)
	BOOLEAN frame_inverted;
#endif

	struct _channel
	{
		DIMENSION width;		//!< Width of the next channel in the bitstream
		DIMENSION height;		//!< Height of the next channel in the bitstream

		//! Precision of the component array for the next channel in the bitstream
		PRECISION bits_per_component;

		//! Number of bits per lowpass coefficient
		PRECISION lowpass_precision;

	} channel[MAX_CHANNEL_COUNT];	//!< Information about each channel

	//! Dimensions and format of the image that was input to the encoder
	struct _input
	{
		DIMENSION width;			//!< Width of the image input to the encoder
		DIMENSION height;			//!< Height of the image input to the encoder
		PIXEL_FORMAT format;		//!< Pixel format of the image input to the encode

	} input;			//!< Information about the image input to the encoder

#if VC5_ENABLED_PART(VC5_PART_LAYERS)
	uint_least8_t layer_count;		//!< Number of subsamples in each sample
#endif

	//! Wavelet tree for each channel
	TRANSFORM transform[MAX_CHANNEL_COUNT];

	//! Codebook to use for encoding
	ENCODER_CODESET *codeset;

	//! Scratch buffer for unpacking the input image
	PIXEL *unpacked_buffer[MAX_CHANNEL_COUNT];

	//! Parameter that controls the amount of rounding before quantization
	int midpoint_prequant;

	//! Table for the order in which channels are encoded into the bitstream
	CHANNEL channel_order_table[MAX_CHANNEL_COUNT];

	//! Number of entries in the channel order table (may be less than the channel count)
	int channel_order_count;

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    uint8_t image_sequence_identifier[16];      //!< UUID used for the unique image identifier
    uint32_t image_sequence_number;             //!< Number of the image in the encoded sequence
#endif

#if VC5_ENABLED_PART(VC5_PART_IMAGE_FORMATS)
    COMPONENT_TRANSFORM *component_transform;
    COMPONENT_PERMUTATION *component_permutation;
#endif

    //! Six rows of horizontal lowpass results for each channel
    PIXEL *lowpass_buffer[MAX_WAVELET_COUNT][ROW_BUFFER_COUNT];
    
    //! Six rows of horizontal highpass results for each channel
    PIXEL *highpass_buffer[MAX_WAVELET_COUNT][ROW_BUFFER_COUNT];
    
#if VC5_ENABLED_PART(VC5_PART_SECTIONS)
    ENABLED_SECTIONS enabled_sections;
#endif
    
} ENCODER;

#ifdef __cplusplus
extern "C" {
#endif

    CODEC_ERROR InitEncoder(ENCODER *encoder, const gpr_allocator *allocator, const VERSION *version);
    
    //TAGWORD PackedEncoderVersion(ENCODER *encoder);
    
    CODEC_ERROR PrepareEncoder(ENCODER *encoder,
                               const UNPACKED_IMAGE *image,
                               const ENCODER_PARAMETERS *parameters);
    
    CODEC_ERROR PrepareEncoderState(ENCODER *encoder,
                                    const UNPACKED_IMAGE *image,
                                    const ENCODER_PARAMETERS *parameters);
    
    CODEC_ERROR SetInputChannelFormats(ENCODER *encoder, ENCODER_PARAMETERS *parameters);
    
    CODEC_ERROR ReleaseEncoder(ENCODER *encoder);
    
    CODEC_ERROR AllocEncoderTransforms(ENCODER *encoder);
    
    CODEC_ERROR AllocEncoderBuffers(ENCODER *encoder);
    
    //CODEC_ERROR EncodeStream(IMAGE *image, STREAM *stream, PARAMETERS *parameters);
    CODEC_ERROR EncodeImage(IMAGE *image, STREAM *stream, RGB_IMAGE *rgb_image, ENCODER_PARAMETERS *parameters);
    
    //CODEC_ERROR EncodeSingleImage(ENCODER *encoder, IMAGE *image, BITSTREAM *stream);
    CODEC_ERROR EncodingProcess(ENCODER *encoder,
                                const UNPACKED_IMAGE *image,
                                BITSTREAM *stream,
                                const ENCODER_PARAMETERS *parameters);
    
    CODEC_ERROR EncodeSingleImage(ENCODER *encoder, const UNPACKED_IMAGE *image, BITSTREAM *stream);
    
    CODEC_ERROR EncodeSingleChannel(ENCODER *encoder, void *buffer, size_t pitch, BITSTREAM *stream);
    
    //CODEC_ERROR EncodeMultipleImages(ENCODER *encoder, IMAGE *image_array[], int frame_count, BITSTREAM *stream);
    
    CODEC_ERROR PrepareEncoderTransforms(ENCODER *encoder);
    
    //CODEC_ERROR ImageUnpackingProcess(ENCODER *encoder, IMAGE *image);
    CODEC_ERROR ImageUnpackingProcess(const PACKED_IMAGE *packed_image,
                                      UNPACKED_IMAGE *unpacked_image,
                                      const ENCODER_PARAMETERS *parameters,
                                      gpr_allocator *allocator);
    
    CODEC_ERROR UnpackImage(const PACKED_IMAGE *input, UNPACKED_IMAGE *output, ENABLED_PARTS enabled_parts);
    
    CODEC_ERROR PreprocessImageRow(uint8_t *input, DIMENSION image_width, uint8_t *output);
    
    CODEC_ERROR UnpackImageRow(uint8_t *input_row_ptr,
                               DIMENSION image_width,
                               PIXEL_FORMAT pixel_format,
                               PIXEL *output_row_ptr[],
                               PRECISION bits_per_component[],
                               int channel_count,
                               ENABLED_PARTS enabled_parts,
                               int raw_shift);
    
    CODEC_ERROR EncodeBitstreamHeader(ENCODER *encoder, BITSTREAM *bitstream);
    
    CODEC_ERROR EncodeBitstreamTrailer(ENCODER *encoder, BITSTREAM *bitstream);
    
    CODEC_ERROR EncodeExtensionHeader(ENCODER *encoder, BITSTREAM *bitstream);
    
    CODEC_ERROR EncodeExtensionTrailer(ENCODER *encoder, BITSTREAM *bitstream);
    
    //CODEC_ERROR EncodeLayer(ENCODER *encoder, void *buffer, size_t pitch, BITSTREAM *stream);
    CODEC_ERROR EncodeMultipleChannels(ENCODER *encoder, const UNPACKED_IMAGE *image, BITSTREAM *stream);
    
#if VC5_ENABLED_PART(VC5_PART_LAYERS)
    CODEC_ERROR EncodeLayerHeader(ENCODER *encoder, BITSTREAM *bitstream);
    CODEC_ERROR EncodeLayerTrailer(ENCODER *encoder, BITSTREAM *bitstream);
#endif
    
    CODEC_ERROR SetEncoderQuantization(ENCODER *encoder,
                                       const ENCODER_PARAMETERS *parameters);
    
    CODEC_ERROR SetTransformQuantTable(ENCODER *encoder, int channel, const QUANT table[], int length);
    
    CODEC_ERROR GetChannelDimensions(ENCODER *encoder,
                                     int channel_number,
                                     DIMENSION *channel_width_out,
                                     DIMENSION *channel_height_out);
    
    CODEC_ERROR GetMaximumChannelDimensions(const UNPACKED_IMAGE *image, DIMENSION *width_out, DIMENSION *height_out);
    
    DIMENSION EncodedLayerHeight(ENCODER *encoder, DIMENSION height);
    
    CODEC_ERROR SetEncodedBandMask(CODEC_STATE *codec, int subband);
    
    CODEC_ERROR EncodeChannelSubbands(ENCODER *encoder, int channel, BITSTREAM *stream);
    
    CODEC_ERROR EncodeChannelHeader(ENCODER *encoder,
                                    int channel_number,
                                    BITSTREAM *stream);
    
    CODEC_ERROR EncodeChannelTrailer(ENCODER *encoder, int channel, BITSTREAM *stream);
    
    //CODEC_ERROR EncodeLayerChannels(ENCODER *encoder, BITSTREAM *stream);
    CODEC_ERROR EncodeChannelWavelets(ENCODER *encoder, BITSTREAM *stream);
    
    CODEC_ERROR PutVideoLowpassHeader(ENCODER *encoder, int channel_number, BITSTREAM *stream);
    
    CODEC_ERROR PutVideoSubbandHeader(ENCODER *encoder, int subband, QUANT quantization, BITSTREAM *stream);
    CODEC_ERROR PutVideoSubbandTrailer(ENCODER *encoder, BITSTREAM *stream);
    
    CODEC_ERROR TransformForwardSpatialQuantFrame(ENCODER *encoder, void *buffer, size_t pitch);
    
    CODEC_ERROR AllocateEncoderHorizontalBuffers(ENCODER *encoder);
    
    CODEC_ERROR DeallocateEncoderHorizontalBuffers(ENCODER *encoder);
    
    CODEC_ERROR AllocateEncoderUnpackingBuffers(ENCODER *encoder, int frame_width);
    
    CODEC_ERROR DeallocateEncoderUnpackingBuffers(ENCODER *encoder);
    
    CODEC_ERROR AllocateHorizontalBuffers(gpr_allocator *allocator,
                                          PIXEL *lowpass_buffer[],
                                          PIXEL *highpass_buffer[],
                                          int buffer_width);
    
    CODEC_ERROR DeallocateHorizontalBuffers(gpr_allocator *allocator,
                                            PIXEL *lowpass_buffer[],
                                            PIXEL *highpass_buffer[]);
    
    
    CODEC_ERROR PadWaveletBands(ENCODER *encoder, WAVELET *wavelet);
    
    CODEC_ERROR EncodeLowpassBand(ENCODER *encoder, WAVELET *wavelet, int channel_number, BITSTREAM *stream);
    
    CODEC_ERROR EncodeHighpassBand(ENCODER *encoder, WAVELET *wavelet, int band, int subband, BITSTREAM *stream);
    
    CODEC_ERROR EncodeHighpassBandLongRuns(BITSTREAM *stream, ENCODER_CODESET *codeset, PIXEL *data,
                                           DIMENSION width, DIMENSION height, DIMENSION pitch);
    
    CODEC_ERROR EncodeHighpassBandRowRuns(BITSTREAM *stream, ENCODER_CODESET *codeset, PIXEL *data,
                                          DIMENSION width, DIMENSION height, DIMENSION pitch);
    
    CODEC_ERROR GetSampleOffsetSegment(BITSTREAM *bitstream, uint32_t offset, TAGVALUE *segment_out);        

#ifdef __cplusplus
}
#endif

#endif // ENCODER_H
