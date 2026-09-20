/*! @file gpr.c
 *
 *  @brief Implementation of top level functions that implement GPR-SDK API
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

#include "gpr.h"

#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_date_time.h"
#include "dng_exceptions.h"
#include "dng_file_stream.h"
#include "dng_globals.h"
#include "dng_host.h"
#include "dng_ifd.h"
#include "dng_image_writer.h"
#include "dng_info.h"
#include "dng_linearization_info.h"
#include "dng_mosaic_info.h"
#include "dng_negative.h"
#include "dng_preview.h"
#include "dng_render.h"
#include "dng_simple_image.h"
#include "dng_tag_codes.h"
#include "dng_tag_types.h"
#include "dng_tag_values.h"
#include "dng_xmp.h"
#include "dng_xmp_sdk.h"
#include "dng_memory_stream.h"
#include "dng_bottlenecks.h"

#include "dng_misc_opcodes.h"
#include "dng_gain_map.h"
#include "dng_lens_correction.h"

#include "gpr_utils.h"

#include "macros.h"
#include "gpr_buffer.h"
#include "gpr_buffer_auto.h"
#include "gpr_flat_write_stream.h"
#include "gpr_rgb.h"

#include "dng_stage1_negative.h"

#if GPR_READING
#include "vc5_decoder.h"
#endif

#if GPR_WRITING
#include "gpr_image_writer.h"
#endif

#if GPR_READING
#include "gpr_read_image.h"
#endif

#include "jpeg.h" // Needed for calling gpr_jpeg_get_dimensions

extern bool gDNGShowTimers;

#define PRINT_MATRIX 0

// Include logging file (for reporting timing)
#include "log.h"

void find_rational(float number, float error_tolerance, int* numerator, int* denominator_pow2)
{
    int _num;
    int _den_pow2;
    
    _den_pow2 = 1;
    
    while(1)
    {
        _num = number * (1 << _den_pow2);
        
        float error = number - (float)_num / (float)((1 << _den_pow2));
        
        if( error < error_tolerance )
            break;
        
        _den_pow2++;
    }
    
    *numerator = _num;
    *denominator_pow2 = _den_pow2;
}

// Convert the sensor black level carried in the metadata into the 16-bit linear domain used
// by WaveletToRGB. The unpack + log-curve pipeline maps a native raw value v to ~v*65535/white
// in that domain, so the black pedestal must be scaled the same way. Returns 0 when there is no
// black level (e.g. GoPro), which makes the subtraction a no-op.
static int compute_rgb_black_level( const gpr_tuning_info* tuning_info )
{
    const gpr_static_black_level& b = tuning_info->static_black_level;

    int black = ( b.r_black + b.g_r_black + b.g_b_black + b.b_black ) / 4;
    int white = tuning_info->dgain_saturation_level.level_red;

    if( black <= 0 || white <= 0 )
        return 0;

    return (int)( (int64_t)black * 65535 / white );
}

// Guarded like the two functions that use it: RGB_PARAMETERS reaches this file
// through vc5_decoder.h (reading) and the image writer (writing), so with both
// features off the type does not exist and neither does any caller.
#if GPR_READING || GPR_WRITING

// The source's OpcodeList2 GainMap opcodes - one per CFA cell, carrying the lens
// shading (vignetting) correction - reduced to one gain grid per output channel for
// WaveletToRGB. Owns the sample storage that RGB_PARAMETERS::shading_map points at,
// so an instance must outlive the render it is handed to.
//
// This is the RGB path's counterpart to what the DNG writer does with the same
// buffers: the writer hands the opcodes to a DNG consumer to apply, while this
// pipeline has to apply them itself. Without it the corners of a GoPro frame render
// up to ~1.9 EV dark everywhere this path is used.
class rgb_shading_map_tables
{
public:

    rgb_shading_map_tables() : _max_gain( 1.0f ) {}

    // Fills rgb_params->shading_map from the four CFA gain maps. Returns false and
    // leaves the correction disabled - render unchanged - for a source that carries
    // no gain maps, or whose four maps do not agree on a single grid geometry.
    bool build( const gpr_tuning_info* tuning_info, RGB_PARAMETERS* rgb_params );

    // The largest gain any channel asks for, i.e. how much shading the source
    // actually withholds. 1.0 for identity maps, which some cameras ship when a
    // capture mode needs no correction (MISSION 1 PRO 12 MP). Only meaningful
    // after a successful build.
    float max_gain() const { return _max_gain; }

private:

    std::vector<float> _samples[3];     // R, G, B

    float              _max_gain;
};

bool rgb_shading_map_tables::build( const gpr_tuning_info* tuning_info, RGB_PARAMETERS* rgb_params )
{
    memset( &rgb_params->shading_map, 0, sizeof(rgb_params->shading_map) );

    const size_t gain_map_size = tuning_info->gain_map.size;

    if( gain_map_size == 0 )
        return false;

    for( int i = 0; i < 4; i++ )
    {
        if( tuning_info->gain_map.buffers[i] == NULL )
            return false;
    }

    // Which cell of the 2x2 CFA tile each opcode describes comes from its own area
    // spec - the (top,left) parity - never from its position in the list, which
    // follows whatever order the source file used and varies by firmware. Greens sit
    // on the tile's main diagonal for patterns that begin with green, on the
    // anti-diagonal for the rest; this is the same test the DNG writer applies.
    bool greens_on_main_diagonal;
    int  red_top;
    int  red_left;

    switch( tuning_info->pixel_format )
    {
        case PIXEL_FORMAT_GBRG_12:      // G B / R G
        case PIXEL_FORMAT_GBRG_12P:
            greens_on_main_diagonal = true;  red_top = 1; red_left = 0;
            break;

        case PIXEL_FORMAT_BGGR_12:      // B G / G R
        case PIXEL_FORMAT_BGGR_14:
            greens_on_main_diagonal = false; red_top = 1; red_left = 1;
            break;

        default:                        // RGGB: R G / G B
            greens_on_main_diagonal = false; red_top = 0; red_left = 0;
            break;
    }

    uint32 points_v = 0, points_h = 0;
    real64 spacing_v = 0.0, spacing_h = 0.0, origin_v = 0.0, origin_h = 0.0;

    int green_count = 0;

    for( int i = 0; i < 4; i++ )
    {
        // Read through dng_stream, the same reader the writer uses on these buffers,
        // so the byte order matches whatever wrote them by construction.
        dng_stream stream( tuning_info->gain_map.buffers[i], (uint32)gain_map_size );

        stream.Get_uint32();                            // version
        stream.Get_uint32();                            // flags
        stream.Get_uint32();                            // opcode data size

        const int32 area_top  = stream.Get_int32();
        const int32 area_left = stream.Get_int32();
        stream.Get_int32();                             // bottom
        stream.Get_int32();                             // right
        stream.Get_uint32();                            // plane
        stream.Get_uint32();                            // planes
        stream.Get_uint32();                            // row pitch
        stream.Get_uint32();                            // col pitch

        const uint32 map_points_v  = stream.Get_uint32();
        const uint32 map_points_h  = stream.Get_uint32();
        const real64 map_spacing_v = stream.Get_real64();
        const real64 map_spacing_h = stream.Get_real64();
        const real64 map_origin_v  = stream.Get_real64();
        const real64 map_origin_h  = stream.Get_real64();
        const uint32 map_planes    = stream.Get_uint32();

        if( map_points_v == 0 || map_points_h == 0 || map_planes == 0 ||
            map_spacing_v <= 0.0 || map_spacing_h <= 0.0 )
        {
            return false;
        }

        const size_t count = (size_t)map_points_v * (size_t)map_points_h;

        if( i == 0 )
        {
            points_v  = map_points_v;  points_h  = map_points_h;
            spacing_v = map_spacing_v; spacing_h = map_spacing_h;
            origin_v  = map_origin_v;  origin_h  = map_origin_h;
        }
        else if( map_points_v  != points_v  || map_points_h  != points_h  ||
                 map_spacing_v != spacing_v || map_spacing_h != spacing_h ||
                 map_origin_v  != origin_v  || map_origin_h  != origin_h )
        {
            // Four maps that do not share a grid cannot be averaged into three
            // channels; leave the render alone rather than guess.
            return false;
        }

        // A CFA source is single-plane, so each opcode carries one map plane; step
        // over any extras rather than assume.
        std::vector<float> gains( count );

        for( size_t k = 0; k < count; k++ )
        {
            gains[k] = stream.Get_real32();

            for( uint32 p = 1; p < map_planes; p++ )
                stream.Get_real32();
        }

        if( ( ( area_top & 1 ) == ( area_left & 1 ) ) == greens_on_main_diagonal )
        {
            // The first green in buffer order rather than an average of the two.
            // The DNG writer copies this same one into both green opcodes (Apple
            // drops the entire gain map when the greens differ), so using it here
            // makes this render agree with what any consumer of the file we write
            // will apply - and keeps the embedded preview byte-identical to a decode
            // of the very GPR it is embedded in, which averaging breaks: the source's
            // greens differ (0.6% on HERO6, up to 7% on MISSION1-50MP) while the
            // written file's are equal by construction.
            if( green_count == 0 )
                _samples[1].swap( gains );

            green_count++;
        }
        else if( ( area_top & 1 ) == red_top && ( area_left & 1 ) == red_left )
        {
            _samples[0].swap( gains );
        }
        else
        {
            _samples[2].swap( gains );
        }
    }

    if( green_count != 2 || _samples[0].empty() || _samples[1].empty() || _samples[2].empty() )
        return false;

    rgb_params->shading_map.samples[0] = &_samples[0][0];
    rgb_params->shading_map.samples[1] = &_samples[1][0];
    rgb_params->shading_map.samples[2] = &_samples[2][0];
    rgb_params->shading_map.points_v   = (int)points_v;
    rgb_params->shading_map.points_h   = (int)points_h;
    rgb_params->shading_map.origin_v   = (float)origin_v;
    rgb_params->shading_map.origin_h   = (float)origin_h;
    rgb_params->shading_map.spacing_v  = (float)spacing_v;
    rgb_params->shading_map.spacing_h  = (float)spacing_h;

    _max_gain = 1.0f;

    for( int c = 0; c < 3; c++ )
    {
        for( size_t k = 0; k < _samples[c].size(); k++ )
        {
            if( _samples[c][k] > _max_gain )
                _max_gain = _samples[c][k];
        }
    }

    return true;
}

#endif // GPR_READING || GPR_WRITING

static void unpack_pixel_format( const gpr_buffer_auto* input_buffer, const gpr_parameters* convert_params, gpr_buffer_auto* output_buffer )
{
    size_t buffer_size = convert_params->input_height * convert_params->input_width * 2;
    output_buffer->allocate( buffer_size );
    
    unsigned char* src = input_buffer->to_uchar();
    uint16_t* dst = output_buffer->to_uint16_t();
    
    int src_stride = convert_params->input_pitch;
    int dst_stride = convert_params->input_width;
    
    for (unsigned int row = 0; row < convert_params->input_height; row++ )
    {
        for (unsigned int col = 0; col < (convert_params->input_width / 2); col++ )
        {
            unsigned char byte_0 = *(src + col * 3 + 0);
            unsigned char byte_1 = *(src + col * 3 + 1);
            unsigned char byte_2 = *(src + col * 3 + 2);
            
            uint16_t pix1 = (byte_2 << 4) + ((byte_1 & 0xf0) >> 4);
            uint16_t pix2 = (byte_0)      + ((byte_1 & 0x0f) << 8);
  
            *(dst + col * 2 + 0) = pix2;
            *(dst + col * 2 + 1) = pix1;
        }
        
        src += src_stride;
        dst += dst_stride;
    }
}

static inline gpr_crop_info parse_crop_info(const dng_ifd &rawIFD, const AutoPtr<dng_negative>& negative )
{
    gpr_crop_info crop_info;

    crop_info.active_area_top    = rawIFD.fActiveArea.t;
    crop_info.active_area_left   = rawIFD.fActiveArea.l;
    crop_info.active_area_bottom = rawIFD.fActiveArea.b;
    crop_info.active_area_right  = rawIFD.fActiveArea.r;

    crop_info.default_crop_origin_h = (uint32_t)( negative->DefaultCropOriginH().As_real64() + 0.5 );
    crop_info.default_crop_origin_v = (uint32_t)( negative->DefaultCropOriginV().As_real64() + 0.5 );

    crop_info.default_crop_size_h   = (uint32_t)( negative->DefaultCropSizeH().As_real64() + 0.5 );
    crop_info.default_crop_size_v   = (uint32_t)( negative->DefaultCropSizeV().As_real64() + 0.5 );
    return crop_info;
}

#if GPR_WRITING

// GPR_QUALITY_SETTING (gpr.h) is the SDK's name for the encoder's VC5_ENCODER_QUALITY_SETTING,
// the way GPR_PIXEL_FORMAT stands for VC5_ENCODER_PIXEL_FORMAT: gpr.h is the customer's header
// and cannot include vc5_encoder.h, which exists only in builds that write. The levels are
// therefore declared twice and tied together here. If the two ever disagree this stops
// compiling, which is what lets set_vc5_encoder_parameters hand the value over with a cast
// instead of a mapping. (A negative array size rather than static_assert, because the SDK
// still builds as C++98.)
#define QUALITY_SETTING_MATCHES( level ) \
    typedef char gpr_quality_setting_##level##_matches[ ( (int)GPR_QUALITY_SETTING_##level == (int)VC5_ENCODER_QUALITY_SETTING_##level ) ? 1 : -1 ]

QUALITY_SETTING_MATCHES( LOW );
QUALITY_SETTING_MATCHES( MEDIUM );
QUALITY_SETTING_MATCHES( HIGH );
QUALITY_SETTING_MATCHES( FS1 );
QUALITY_SETTING_MATCHES( FSX );
QUALITY_SETTING_MATCHES( FS2 );
QUALITY_SETTING_MATCHES( ULTRA );
QUALITY_SETTING_MATCHES( COUNT );
QUALITY_SETTING_MATCHES( DEFAULT );

#undef QUALITY_SETTING_MATCHES

// A quality outside the enum would reach the encoder's table lookup as-is: vc5_encoder_process
// silently keeps its built-in table for a value past the end, and a negative one reads out of
// bounds on compilers that type enums as int. The conversion fails instead.
static bool quality_is_valid( const gpr_parameters* parameters )
{
    if( (unsigned int)parameters->quality < (unsigned int)GPR_QUALITY_SETTING_COUNT )
        return true;

    LogPrint( "Invalid quality setting %d (valid: 0 to %d)", (int)parameters->quality, (int)GPR_QUALITY_SETTING_COUNT - 1 );

    return false;
}

// shading_tables backs the preview's lens shading correction and is only read when the
// encode runs, so the caller owns it and must keep it alive until EncodeVc5Image returns.
static void set_vc5_encoder_parameters( vc5_encoder_parameters& vc5_encoder_params, const gpr_parameters* convert_params,
                                        rgb_shading_map_tables& shading_tables )
{
    vc5_encoder_params.input_width      = convert_params->input_width;
    vc5_encoder_params.input_height     = convert_params->input_height;
    vc5_encoder_params.input_pitch      = convert_params->input_pitch;
    
    switch ( convert_params->tuning_info.pixel_format )
    {
        case PIXEL_FORMAT_RGGB_12:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_12;
            break;

        case PIXEL_FORMAT_RGGB_12P:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_12P;
            break;
            
        case PIXEL_FORMAT_RGGB_14:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_14;
            break;
            
        case PIXEL_FORMAT_GBRG_12:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_GBRG_12;
            break;
            
        case PIXEL_FORMAT_GBRG_12P:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_GBRG_12P;
            break;

        case PIXEL_FORMAT_BGGR_12:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_BGGR_12;
            break;

        case PIXEL_FORMAT_BGGR_14:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_BGGR_14;
            break;

        default:
            break;
    }
    
    vc5_encoder_params.quality_setting = (VC5_ENCODER_QUALITY_SETTING)convert_params->quality;

    // Resolution and rendering parameters of the embedded preview. The preview pipeline
    // consumes the RGB output as 8-bit (it is re-encoded as JPEG), so rgb_bits stays at 8.
    RGB_PARAMETERS& rgb_params = vc5_encoder_params.rgb_params;
    rgb_params.resolution = convert_params->preview_resolution;
    rgb_params.bits = 8;

    // Drive the preview's white balance from the image metadata (rather than the
    // hardcoded GoPro defaults), so previews of non-GoPro sources (e.g. iPhone) are
    // not colour-cast. Mirrors the gain setup on the decode path.
    gpr_rgb_gain& rgb_gain = rgb_params.white_balance_gain;
    find_rational( convert_params->tuning_info.wb_gains.r_gain, 0.125, &rgb_gain.r_gain_num, &rgb_gain.r_gain_pow2_den );
    find_rational( convert_params->tuning_info.wb_gains.g_gain, 0.125, &rgb_gain.g_gain_num, &rgb_gain.g_gain_pow2_den );
    find_rational( convert_params->tuning_info.wb_gains.b_gain, 0.125, &rgb_gain.b_gain_num, &rgb_gain.b_gain_pow2_den );

    // Remove the sensor black pedestal before the gains are applied, otherwise it tints the preview.
    rgb_params.black_level = compute_rgb_black_level( &convert_params->tuning_info );

    // Camera -> sRGB color matrix and baseline exposure from the DNG metadata, so the
    // embedded preview renders with the same colors as the RGB decode path (which sets
    // the same fields on the decoder in gpr_convert_gpr_to_rgb, including the rationale
    // for skipping a positive exposure when the source carries GainMap opcodes).
    compute_camera_to_srgb_color_matrix( &convert_params->tuning_info, &convert_params->profile_info, rgb_params.color_matrix );

    // Lens shading correction from the source's GainMap opcodes, and the baseline
    // exposure that goes with it (see the same pair in gpr_convert_gpr_to_rgb).
    {
        const bool shading_applied = shading_tables.build( &convert_params->tuning_info, &rgb_params );

        const bool maps_carry_gain = shading_applied ? ( shading_tables.max_gain() > 1.01f )
                                                     : convert_params->tuning_info.has_opcode_gain_maps;

        float baseline_exposure = (float)convert_params->tuning_info.baseline_exposure;

        if ( maps_carry_gain && baseline_exposure > 0.0f )
        {
            baseline_exposure = 0.0f;
        }

        rgb_params.baseline_exposure = baseline_exposure;
    }
}
#endif

void gpr_parameters_set_defaults(gpr_parameters* x)
{
    memset(x, 0, sizeof(gpr_parameters));
    
    gpr_exif_info_set_defaults(&x->exif_info);
    gpr_profile_info_set_defaults(&x->profile_info);
    gpr_tuning_info_set_defaults(&x->tuning_info);

    x->enable_preview = true;

    x->preview_resolution = GPR_RGB_RESOLUTION_DEFAULT;

    x->quality = GPR_QUALITY_SETTING_DEFAULT;

    x->reencode = false;

    x->compute_md5sum = false;
}

void gpr_parameters_construct_copy(const gpr_parameters* y, gpr_parameters* x, gpr_malloc mem_alloc)
{
    gpr_parameters_set_defaults(x);
    
    *x = *y;
    
    if( y->gpmf_payload.size > 0 && y->gpmf_payload.buffer != NULL )
    {
        x->gpmf_payload.buffer = mem_alloc( y->gpmf_payload.size );
        memcpy( x->gpmf_payload.buffer, y->gpmf_payload.buffer, y->gpmf_payload.size );
    }
    
    if( y->tuning_info.gain_map.size > 0 )
    {
        for ( int i = 0; i < 4; i++)
        {
            if( y->tuning_info.gain_map.buffers[i] != 0 )
            {
                x->tuning_info.gain_map.buffers[i] = (char*)mem_alloc( y->tuning_info.gain_map.size );
                memcpy( x->tuning_info.gain_map.buffers[i], y->tuning_info.gain_map.buffers[i], y->tuning_info.gain_map.size );
            }
        }
    }
}

void gpr_parameters_destroy(gpr_parameters* x, gpr_free mem_free)
{
    if( x->gpmf_payload.buffer && x->gpmf_payload.size > 0 )
    {
        mem_free( x->gpmf_payload.buffer );
    }
    
    if( x->tuning_info.gain_map.size > 0 )
    {
        for ( int i = 0; i < 4; i++)
        {
            if( x->tuning_info.gain_map.buffers[i] )
                mem_free( x->tuning_info.gain_map.buffers[i] );
        }
    }
}


static dng_srational convert_to_dng_srational(gpr_signed_rational x)
{
    return dng_srational(x.numerator, x.denominator);
}

static dng_urational convert_to_dng_urational(gpr_unsigned_rational x)
{
    return dng_urational(x.numerator, x.denominator);
}

static gpr_unsigned_rational convert_to_unsigned_rational(dng_urational x)
{
    gpr_unsigned_rational a;
    a.numerator = x.n;
    a.denominator = x.d;
    
    return a;
}

static gpr_signed_rational convert_to_signed_rational(dng_srational x)
{
    gpr_signed_rational a;
    a.numerator = x.n;
    a.denominator = x.d;
    
    return a;
}

static dng_date_time convert_to_dng_date_time( const gpr_date_and_time& x )
{
    return dng_date_time (x.year, x.month, x.day, x.hour, x.minute, x.second );
}

static gpr_date_and_time convert_to_dng_date_and_time( const dng_date_time& x )
{
    gpr_date_and_time a;
    
    a.year = x.fYear;
    a.month = x.fMonth;
    a.day = x.fDay;
    a.hour = x.fHour;
    a.minute = x.fMinute;
    a.second = x.fSecond;
    
    return a;
}

static void add_preview_to_list(dng_host& host, dng_preview_list*& preview_list, const void* preview_buffer, unsigned int preview_h, unsigned int preview_w, unsigned int preview_size )
{
    if( preview_list == NULL )
        preview_list = new dng_preview_list;

    AutoPtr<dng_jpeg_preview> jpeg_preview;
    jpeg_preview.Reset(new dng_jpeg_preview);
    jpeg_preview->fPhotometricInterpretation = piYCbCr;
    jpeg_preview->fInfo.fIsPrimary = true;

    jpeg_preview->fPreviewSize.v             = preview_h;
    jpeg_preview->fPreviewSize.h             = preview_w;
    jpeg_preview->fCompressedData.Reset(host.Allocate( preview_size ));
    memcpy( jpeg_preview->fCompressedData->Buffer_char(), preview_buffer, preview_size );
    
    AutoPtr<dng_preview> pp( dynamic_cast<dng_preview*>(jpeg_preview.Release()) );
    preview_list->Append(pp);
}

static void convert_dng_exif_info_to_dng_exif( dng_exif* dst_exif, const gpr_exif_info* src_exif )
{
    dst_exif->fModel.Set_ASCII( src_exif->camera_model );
    dst_exif->fMake.Set_ASCII( src_exif->camera_make );
    dst_exif->fCameraSerialNumber.Set_ASCII( src_exif->camera_serial );
    dst_exif->fImageDescription.Set_ASCII( src_exif->image_description );
        
    dst_exif->fApertureValue       = convert_to_dng_urational( src_exif->aperture );
    dst_exif->fMaxApertureValue    = convert_to_dng_urational( src_exif->aperture );
    dst_exif->fFNumber             = convert_to_dng_urational( src_exif->f_stop_number );
    dst_exif->fExposureTime        = convert_to_dng_urational( src_exif->exposure_time );
    dst_exif->fFocalLength         = convert_to_dng_urational( src_exif->focal_length );
    dst_exif->fDigitalZoomRatio    = convert_to_dng_urational( src_exif->digital_zoom );
    dst_exif->fExposureBiasValue   = convert_to_dng_srational( src_exif->exposure_bias );
    
    const int32_t focal_plane = 72;
    dst_exif->fFocalPlaneXResolution    = dng_urational(focal_plane, 1);
    dst_exif->fFocalPlaneYResolution    = dng_urational(focal_plane, 1);
    
    dst_exif->fMeteringMode             = src_exif->metering_mode;
    dst_exif->fFocalLengthIn35mmFilm    = src_exif->focal_length_in_35mm_film;
    dst_exif->fExposureProgram          = src_exif->exposure_program;
    dst_exif->fLightSource              = src_exif->light_source;
    dst_exif->fFlash                    = src_exif->flash;
    dst_exif->fSensingMethod            = src_exif->sensing_method;
    dst_exif->fFileSource               = src_exif->file_source;
    dst_exif->fSceneType                = src_exif->scene_type;
    dst_exif->fWhiteBalance             = src_exif->white_balance;
    dst_exif->fExposureMode             = src_exif->exposure_mode;
    dst_exif->fSceneCaptureType         = src_exif->scene_capture_type;
    dst_exif->fGainControl              = src_exif->gain_control;
    dst_exif->fContrast                 = src_exif->contrast;
    dst_exif->fSaturation               = src_exif->saturation;
    dst_exif->fSharpness                = src_exif->sharpness;
    
    dst_exif->fISOSpeedRatings[0]       = src_exif->iso_speed_rating;
    
    dst_exif->fComponentsConfiguration  = 0x04050600; //?? - since we're raw?
    
    dst_exif->fDateTimeOriginal.SetDateTime( convert_to_dng_date_time( src_exif->date_time_original ) );
    dst_exif->fDateTimeDigitized.SetDateTime( convert_to_dng_date_time( src_exif->date_time_digitized ) );
    
    dst_exif->fSoftware.Set(src_exif->software_version);
    
    dst_exif->fUserComment.Set(src_exif->user_comment);
    
    const gpr_gps_info& src_gps_info = src_exif->gps_info;

    if( src_gps_info.gps_info_valid )
    {
        dst_exif->fGPSVersionID = src_gps_info.version_id;
        
        dst_exif->fGPSLatitudeRef.Set( src_gps_info.latitude_ref );
        
        dst_exif->fGPSLatitude[0] = convert_to_dng_urational( src_gps_info.latitude[0] );
        dst_exif->fGPSLatitude[1] = convert_to_dng_urational( src_gps_info.latitude[1] );
        dst_exif->fGPSLatitude[2] = convert_to_dng_urational( src_gps_info.latitude[2] );
        
        dst_exif->fGPSLongitudeRef.Set( src_gps_info.longitude_ref );
        
        dst_exif->fGPSLongitude[0] = convert_to_dng_urational( src_gps_info.longitude[0] );
        dst_exif->fGPSLongitude[1] = convert_to_dng_urational( src_gps_info.longitude[1] );
        dst_exif->fGPSLongitude[2] = convert_to_dng_urational( src_gps_info.longitude[2] );
        
        dst_exif->fGPSAltitudeRef = src_gps_info.altitude_ref;
        
        dst_exif->fGPSAltitude = convert_to_dng_urational( src_gps_info.altitude );
        
        dst_exif->fGPSTimeStamp[0] = convert_to_dng_urational( src_gps_info.time_stamp[0] );
        dst_exif->fGPSTimeStamp[1] = convert_to_dng_urational( src_gps_info.time_stamp[1] );
        dst_exif->fGPSTimeStamp[2] = convert_to_dng_urational( src_gps_info.time_stamp[2] );
        
        dst_exif->fGPSSatellites.Set( src_gps_info.satellites );
        
        dst_exif->fGPSStatus.Set( src_gps_info.status );
        
        dst_exif->fGPSMeasureMode.Set( src_gps_info.measure_mode );
        
        dst_exif->fGPSDOP = convert_to_dng_urational( src_gps_info.dop );
        
        dst_exif->fGPSSpeedRef.Set( src_gps_info.speed_ref );
        
        dst_exif->fGPSSpeed = convert_to_dng_urational( src_gps_info.speed );
        
        dst_exif->fGPSTrackRef.Set( src_gps_info.track_ref );
        
        dst_exif->fGPSTrack = convert_to_dng_urational( src_gps_info.track );
        
        dst_exif->fGPSImgDirectionRef.Set( src_gps_info.img_direction_ref );
        
        dst_exif->fGPSImgDirection = convert_to_dng_urational( src_gps_info.img_direction );
        
        dst_exif->fGPSMapDatum.Set( src_gps_info.map_datum );
        
        dst_exif->fGPSDestLatitudeRef.Set( src_gps_info.dest_latitude_ref );
        
        dst_exif->fGPSDestLatitude[0] = convert_to_dng_urational( src_gps_info.dest_latitude[0] );
        dst_exif->fGPSDestLatitude[1] = convert_to_dng_urational( src_gps_info.dest_latitude[1] );
        dst_exif->fGPSDestLatitude[2] = convert_to_dng_urational( src_gps_info.dest_latitude[2] );
        
        dst_exif->fGPSDestLongitudeRef.Set( src_gps_info.dest_longitude_ref );
        
        dst_exif->fGPSDestLongitude[0] = convert_to_dng_urational( src_gps_info.dest_longitude[0] );
        dst_exif->fGPSDestLongitude[1] = convert_to_dng_urational( src_gps_info.dest_longitude[1] );
        dst_exif->fGPSDestLongitude[2] = convert_to_dng_urational( src_gps_info.dest_longitude[2] );
        
        dst_exif->fGPSDestBearingRef.Set( src_gps_info.dest_bearing_ref );
        
        dst_exif->fGPSDestBearing = convert_to_dng_urational( src_gps_info.dest_bearing );
        
        dst_exif->fGPSDestDistanceRef.Set( src_gps_info.dest_distance_ref );
        
        dst_exif->fGPSDestDistance = convert_to_dng_urational( src_gps_info.dest_distance );
        
        dst_exif->fGPSProcessingMethod.Set( src_gps_info.processing_method );
        
        dst_exif->fGPSAreaInformation.Set( src_gps_info.area_information );
        
        dst_exif->fGPSDateStamp.Set( src_gps_info.date_stamp );
        
        dst_exif->fGPSDifferential = src_gps_info.differential;
    }
}

static void convert_dng_exif_to_dng_exif_info( gpr_exif_info* dst_exif, const dng_exif* src_exif )
{
    assert(src_exif->fModel.Length() < sizeof(dst_exif->camera_model));
    strcpy( dst_exif->camera_model, src_exif->fModel.Get() );

    assert(src_exif->fMake.Length() < sizeof(dst_exif->camera_make));
    strcpy( dst_exif->camera_make, src_exif->fMake.Get() );

    assert(src_exif->fCameraSerialNumber.Length() < sizeof(dst_exif->camera_serial));
    strcpy( dst_exif->camera_serial, src_exif->fCameraSerialNumber.Get() );

    assert(src_exif->fImageDescription.Length() < sizeof(dst_exif->image_description));
    strcpy( dst_exif->image_description, src_exif->fImageDescription.Get() );
    
    dst_exif->aperture          = convert_to_unsigned_rational( src_exif->fMaxApertureValue );
    dst_exif->f_stop_number     = convert_to_unsigned_rational( src_exif->fFNumber );
    dst_exif->exposure_time     = convert_to_unsigned_rational( src_exif->fExposureTime );
    dst_exif->focal_length      = convert_to_unsigned_rational( src_exif->fFocalLength );
    dst_exif->digital_zoom      = convert_to_unsigned_rational( src_exif->fDigitalZoomRatio );
    dst_exif->exposure_bias     = convert_to_signed_rational( src_exif->fExposureBiasValue );
    
    dst_exif->metering_mode             = (gpr_metering_mode)src_exif->fMeteringMode;
    dst_exif->focal_length_in_35mm_film = src_exif->fFocalLengthIn35mmFilm;
    dst_exif->exposure_program          = (gpr_exposure_program)src_exif->fExposureProgram;
    dst_exif->light_source              = (gpr_light_source)src_exif->fLightSource;
    dst_exif->flash                     = (gpr_flash)src_exif->fFlash;
    dst_exif->sensing_method            = (gpr_sensing_method)src_exif->fSensingMethod;
    dst_exif->file_source               = (gpr_file_source)src_exif->fFileSource;
    dst_exif->scene_type                = (gpr_scene_type)src_exif->fSceneType;
    dst_exif->white_balance             = (gpr_white_balance)src_exif->fWhiteBalance;
    dst_exif->exposure_mode             = (gpr_exposure_mode)src_exif->fExposureMode;
    dst_exif->scene_capture_type        = (gpr_scene_capture_type)src_exif->fSceneCaptureType;
    dst_exif->gain_control              = (gpr_gain_control)src_exif->fGainControl;
    dst_exif->contrast                  = (gpr_contrast)src_exif->fContrast;
    dst_exif->saturation                = src_exif->fSaturation;
    dst_exif->sharpness                 = (gpr_sharpness)src_exif->fSharpness;
    
    dst_exif->iso_speed_rating          = src_exif->fISOSpeedRatings[0];
    
    dst_exif->date_time_original        = convert_to_dng_date_and_time( src_exif->fDateTimeOriginal.DateTime() );
    dst_exif->date_time_digitized       = convert_to_dng_date_and_time( src_exif->fDateTimeOriginal.DateTime() );
    
    assert(src_exif->fSoftware.Length() < sizeof(dst_exif->software_version));
    memcpy( dst_exif->software_version, src_exif->fSoftware.Get(), src_exif->fSoftware.Length() );

    assert(src_exif->fUserComment.Length() < sizeof(dst_exif->user_comment));
    memcpy( dst_exif->user_comment, src_exif->fUserComment.Get(), src_exif->fUserComment.Length() );
    
    // GPS Info
    gpr_gps_info& dst_gps_info = dst_exif->gps_info;
    
    dst_gps_info.version_id = src_exif->fGPSVersionID;

    dst_gps_info.gps_info_valid = dst_gps_info.version_id > 0;
    
    strcpy( dst_gps_info.latitude_ref, src_exif->fGPSLatitudeRef.Get() );
    
    dst_gps_info.latitude[0] = convert_to_unsigned_rational( src_exif->fGPSLatitude[0] );
    dst_gps_info.latitude[1] = convert_to_unsigned_rational( src_exif->fGPSLatitude[1] );
    dst_gps_info.latitude[2] = convert_to_unsigned_rational( src_exif->fGPSLatitude[2] );

    strcpy( dst_gps_info.longitude_ref, src_exif->fGPSLongitudeRef.Get() );
    
    dst_gps_info.longitude[0] = convert_to_unsigned_rational( src_exif->fGPSLongitude[0] );
    dst_gps_info.longitude[1] = convert_to_unsigned_rational( src_exif->fGPSLongitude[1] );
    dst_gps_info.longitude[2] = convert_to_unsigned_rational( src_exif->fGPSLongitude[2] );
    
    dst_gps_info.altitude_ref = src_exif->fGPSAltitudeRef;
    
    dst_gps_info.altitude = convert_to_unsigned_rational( src_exif->fGPSAltitude );
    
    dst_gps_info.time_stamp[0] = convert_to_unsigned_rational( src_exif->fGPSTimeStamp[0] );
    dst_gps_info.time_stamp[1] = convert_to_unsigned_rational( src_exif->fGPSTimeStamp[1] );
    dst_gps_info.time_stamp[2] = convert_to_unsigned_rational( src_exif->fGPSTimeStamp[2] );
    
    strcpy( dst_gps_info.satellites, src_exif->fGPSSatellites.Get() );
    
    strcpy( dst_gps_info.status, src_exif->fGPSStatus.Get() );
    
    strcpy( dst_gps_info.measure_mode, src_exif->fGPSMeasureMode.Get() );

    dst_gps_info.dop = convert_to_unsigned_rational( src_exif->fGPSDOP );
    
    strcpy( dst_gps_info.speed_ref, src_exif->fGPSSpeedRef.Get() );
    
    dst_gps_info.speed = convert_to_unsigned_rational( src_exif->fGPSSpeed );
    
    strcpy( dst_gps_info.track_ref, src_exif->fGPSTrackRef.Get() );
    
    dst_gps_info.track = convert_to_unsigned_rational( src_exif->fGPSTrack );
    
    strcpy( dst_gps_info.img_direction_ref,  src_exif->fGPSImgDirectionRef.Get() );
    
    dst_gps_info.img_direction = convert_to_unsigned_rational( src_exif->fGPSImgDirection );
    
    strcpy( dst_gps_info.map_datum, src_exif->fGPSMapDatum.Get() );
    
    strcpy( dst_gps_info.dest_latitude_ref, src_exif->fGPSDestLatitudeRef.Get() );
    
    dst_gps_info.dest_latitude[0] = convert_to_unsigned_rational( src_exif->fGPSDestLatitude[0] );
    dst_gps_info.dest_latitude[1] = convert_to_unsigned_rational( src_exif->fGPSDestLatitude[1] );
    dst_gps_info.dest_latitude[2] = convert_to_unsigned_rational( src_exif->fGPSDestLatitude[2] );

    strcpy( dst_gps_info.dest_longitude_ref, src_exif->fGPSDestLongitudeRef.Get() );
    
    dst_gps_info.dest_longitude[0] = convert_to_unsigned_rational( src_exif->fGPSDestLongitude[0] );
    dst_gps_info.dest_longitude[1] = convert_to_unsigned_rational( src_exif->fGPSDestLongitude[1] );
    dst_gps_info.dest_longitude[2] = convert_to_unsigned_rational( src_exif->fGPSDestLongitude[2] );
    
    strcpy( dst_gps_info.dest_bearing_ref, src_exif->fGPSDestBearingRef.Get() );
    
    dst_gps_info.dest_bearing = convert_to_unsigned_rational( src_exif->fGPSDestBearing );
    
    strcpy( dst_gps_info.dest_distance_ref, src_exif->fGPSDestDistanceRef.Get() );
    
    dst_gps_info.dest_distance = convert_to_unsigned_rational( src_exif->fGPSDestDistance );
    
    strcpy( dst_gps_info.processing_method, src_exif->fGPSProcessingMethod.Get() );
    
    strcpy( dst_gps_info.area_information, src_exif->fGPSAreaInformation.Get() );
    
    strcpy( dst_gps_info.date_stamp, src_exif->fGPSDateStamp.Get() );
    
    dst_gps_info.differential = src_exif->fGPSDifferential;
}


#define MAX_BUF_SIZE (65*65*4*sizeof(float))

// Exposes the protected warp parameters of a WarpRectilinear opcode reconstructed
// from its serialized form (the class offers no public accessor).
class gpr_warp_rectilinear_reader : public dng_opcode_WarpRectilinear
{
public:
    explicit gpr_warp_rectilinear_reader( dng_stream &stream ) : dng_opcode_WarpRectilinear( stream ) {}

    const dng_warp_params_rectilinear& Params() const { return fWarpParams; }
};

// Decodes a WarpRectilinear opcode into tuning_info's warp struct by serializing it
// (same framing as OpcodeList storage: minVersion, flags, then PutData) and reparsing.
// Leaves warp.planes == 0 if the opcode cannot be represented.
static void read_warp_rectilinear_opcode( dng_opcode &opcode, gpr_warp_rectilinear &warp )
{
    if ( opcode.OpcodeID() != dngOpcode_WarpRectilinear )
        return;

    try
    {
        char buffer [256];

        dng_stream stream ( buffer, sizeof(buffer) );

        stream.Put_uint32 ( opcode.MinVersion() );
        stream.Put_uint32 ( opcode.Flags() );

        opcode.PutData( stream );

        stream.SetReadPosition( 0 );

        gpr_warp_rectilinear_reader reader ( stream );

        const dng_warp_params_rectilinear &params = reader.Params();

        if ( params.fPlanes > GPR_WARP_MAX_PLANES )
            return;

        warp.planes   = params.fPlanes;
        warp.flags    = opcode.Flags();
        warp.center_x = params.fCenter.h;
        warp.center_y = params.fCenter.v;

        for ( uint32 p = 0; p < params.fPlanes; p++ )
        {
            for ( int i = 0; i < 4; i++ )
                warp.radial[p][i] = params.fRadParams[p][i];

            warp.tangential[p][0] = params.fTanParams[p][0];
            warp.tangential[p][1] = params.fTanParams[p][1];
        }
    }
    catch ( ... )
    {
        warp.planes = 0;
    }
}

// The parameters to write with, once a preview has been found for a conversion that carried
// none of its own: `parameters` unchanged when there is nothing to attach, otherwise `scratch`
// - a shallow copy carrying the JPEG, owning nothing. preview_jpg keeps ownership, and has to
// outlive the write.
static const gpr_parameters* attach_preview( const gpr_parameters*  parameters,
                                                   gpr_parameters*  scratch,
                                             const gpr_buffer_auto* preview_jpg )
{
    if( parameters->preview_image.jpg_preview.buffer != NULL || preview_jpg->is_valid() == false )
        return parameters;

    *scratch = *parameters;
    scratch->preview_image.jpg_preview.buffer = preview_jpg->get_buffer();
    scratch->preview_image.jpg_preview.size   = preview_jpg->get_size();

    return scratch;
}

// The file's embedded JPEG thumbnail, copied out of whichever IFD holds it, for a conversion
// that would rather carry the preview across than decode a new one. Any JPEG-compressed IFD
// that is not the raw one qualifies; in a DNG that is IFD 0. Leaves the buffer empty when the
// file has no preview - which is every GPR straight off a GoPro.
static void read_preview_jpg( dng_info& info, dng_stream& stream, gpr_buffer_auto* out_preview_jpg )
{
    for( uint32 i = 0; i < info.fIFDCount; i++ )
    {
        const dng_ifd* ifd = info.fIFD[i].Get();

        if( ifd == NULL || (int32)i == info.fMainIndex )
            continue;

        if( ( ifd->fCompression != ccJPEG && ifd->fCompression != ccOldJPEG ) ||
            ifd->fTileOffsetsCount != 1 || ifd->fTileByteCount[0] == 0 )
        {
            continue;
        }

        // A strip that runs past the end of the file is a malformed input, not a preview;
        // reading it would throw out of a call whose callers expect a bool.
        if( ifd->fTileOffset[0] + ifd->fTileByteCount[0] > stream.Length() )
            continue;

        out_preview_jpg->allocate( ifd->fTileByteCount[0] );

        if( out_preview_jpg->is_valid() )
        {
            stream.SetReadPosition( ifd->fTileOffset[0] );
            stream.Get( out_preview_jpg->get_buffer(), ifd->fTileByteCount[0] );
        }

        return;
    }
}

static bool read_dng(const gpr_allocator*       allocator,
                           dng_stream*          dng_read_stream,
                           gpr_buffer_auto*     raw_image_buffer,
                           gpr_buffer_auto*     vc5_image_buffer,
                           gpr_parameters*      convert_params = NULL,
                           bool*                is_vc5_format = NULL,
                           AutoPtr<dng_image>*  out_decoded_image = NULL,
                           gpr_buffer_auto*     out_preview_jpg = NULL )
{
    dng_host host;
    
    gpr_buffer_auto vc5_image_obj( allocator->Alloc, allocator->Free );
    
    static uint32 gPreferredSize = 0;
    static uint32 gMinimumSize   = 0;
    static uint32 gMaximumSize   = 0;
    
    static dng_string gDumpDNG;
    
    host.SetPreferredSize (gPreferredSize);
    host.SetMinimumSize   (gMinimumSize  );
    host.SetMaximumSize   (gMaximumSize  );
    
    host.ValidateSizes ();
    
    host.SetSaveDNGVersion (dngVersion_SaveDefault);
    
    host.SetSaveLinearDNG (false);
    
    host.SetKeepOriginalFile (false);
    
    AutoPtr<dng_negative> negative;

    bool stage1_read = false;

    if( raw_image_buffer != NULL && vc5_image_buffer == NULL )
    {
        vc5_image_buffer = &vc5_image_obj;
    }
    
    {
        dng_info info;
        
        info.Parse (host, *dng_read_stream);
        
        info.PostParse (host);
        
        if (!info.IsValidDNG ())
        {
            return false;
        }

        if( out_preview_jpg != NULL )
        {
            read_preview_jpg( info, *dng_read_stream, out_preview_jpg );
        }

        dng_memory_block* gpmf_payload = host.GetGPMFPayload().Get();
        
        if( gpmf_payload && gpmf_payload->LogicalSize() > 0 )
        {
            if( convert_params != NULL && convert_params->gpmf_payload.buffer == NULL && convert_params->gpmf_payload.size == 0 )
            {
                convert_params->gpmf_payload.size   = gpmf_payload->LogicalSize();
                convert_params->gpmf_payload.buffer = allocator->Alloc( convert_params->gpmf_payload.size );
                
                memcpy( convert_params->gpmf_payload.buffer, gpmf_payload->Buffer(), convert_params->gpmf_payload.size );
            }
        }
        
        negative.Reset (dng_stage1_negative::Make (host));
        
        negative->Parse (host, *dng_read_stream, info);
        
        negative->PostParse (host, *dng_read_stream, info);
        
#if GPR_READING
        if( negative->IsVc5Image( info ) )
        {
            gpr_read_image reader( vc5_image_buffer );
            
            if( vc5_image_buffer == NULL )
            {
                reader.SetReadVC5(false);
            }
            
            if( raw_image_buffer == NULL )
            {
                reader.SetDecodeVC5(false);
            }
            
            negative->ReadVc5Image(host, *dng_read_stream, info, reader);

            dng_ifd &rawIFD = *info.fIFD [info.fMainIndex].Get ();
	        
            if (rawIFD.fOpcodeList2Count)
            {
                negative->OpcodeList2().Parse (host, *dng_read_stream, rawIFD.fOpcodeList2Count, rawIFD.fOpcodeList2Offset);
            }
	        
            if (rawIFD.fOpcodeList3Count)
            {
                negative->OpcodeList3().Parse (host, *dng_read_stream, rawIFD.fOpcodeList3Count, rawIFD.fOpcodeList3Offset);
            }
	        
            if( is_vc5_format )
                *is_vc5_format = true;
        }
        else
#endif
        {
            negative->ReadStage1Image (host, *dng_read_stream, info);

            stage1_read = true;

            if( is_vc5_format )
                *is_vc5_format = false;
        }

        const dng_image& raw_image = negative->RawImage();
        
        if( convert_params )
        {
            dng_rect bounds = raw_image.Bounds();
            int i,j;
            
            convert_params->input_width  = bounds.W();
            convert_params->input_height = bounds.H();
            convert_params->input_pitch  = convert_params->input_width * 2;
            
            // Copy ColorMatrix1, ColorMatrix2
            {
                const dng_camera_profile &profile_info = negative->ProfileByIndex( 0 );
                const dng_matrix &m1 = profile_info.ColorMatrix1();
                const dng_matrix &m2 = profile_info.ColorMatrix2();
                
                for (i = 0; i < 3; i++)
                {
                    for (j = 0; j < 3; j++)
                    {
                        convert_params->profile_info.color_matrix_1[i][j] = m1[i][j];
                        convert_params->profile_info.color_matrix_2[i][j] = m2[i][j];
                    }
                }

                // Read the calibration illuminants from the same profile so they stay
                // paired with the color matrices above (otherwise they keep stale defaults).
                convert_params->profile_info.illuminant1 = profile_info.CalibrationIlluminant1();
                convert_params->profile_info.illuminant2 = profile_info.CalibrationIlluminant2();

                convert_params->profile_info.compute_color_matrix = false;
                convert_params->profile_info.matrix_weighting = 1.0;
                
                memset( convert_params->profile_info.wb1, 0, sizeof(convert_params->profile_info.wb1) );
                memset( convert_params->profile_info.wb2, 0, sizeof(convert_params->profile_info.wb2) );
                
                memset( convert_params->profile_info.cam_to_srgb_1, 0, sizeof(convert_params->profile_info.cam_to_srgb_1) );
                memset( convert_params->profile_info.cam_to_srgb_2, 0, sizeof(convert_params->profile_info.cam_to_srgb_2) );
            }
            
            // Set Exif Info
            convert_dng_exif_to_dng_exif_info( &convert_params->exif_info, negative->GetExif() );
            
            // Set Tuning Info
            {
                gpr_tuning_info& tuning_info = convert_params->tuning_info;
                
                tuning_info.orientation = (GPR_ORIENTATION)negative->BaseOrientation().GetAdobe();
                
                if( negative->HasCameraNeutral() )
                {
                    const dng_vector& camNeutral = negative->CameraNeutral();

                    tuning_info.wb_gains.r_gain = 1 / camNeutral[0];
                    tuning_info.wb_gains.g_gain = 1 / camNeutral[1];
                    tuning_info.wb_gains.b_gain = 1 / camNeutral[2];
                }

                tuning_info.baseline_exposure  = negative->BaselineExposure();

                // GainMap opcodes present? Their per-area gains are folded into how GoPro
                // chose BaselineExposure, so renderers that skip opcodes (this SDK, and
                // Apple's, which rejects GoPro's maps outright) misexpose if they apply
                // BaselineExposure alone. Recorded so the RGB decode can skip it too.
                // Specifically GainMap (opcode 9): the Warp opcodes GoPro writes in every
                // fisheye file are geometric and say nothing about exposure.
                {
                    const dng_opcode_list* lists[3] = { &negative->OpcodeList1(),
                                                        &negative->OpcodeList2(),
                                                        &negative->OpcodeList3() };
                    tuning_info.has_opcode_gain_maps = false;
                    for ( uint32 list_index = 0; list_index < 3; list_index++ )
                    {
                        for ( uint32 k = 0; k < lists[list_index]->Count(); k++ )
                        {
                            if ( lists[list_index]->Entry( k ).OpcodeID() == dngOpcode_GainMap )
                            {
                                tuning_info.has_opcode_gain_maps = true;
                            }
                        }
                    }
                }

                tuning_info.baseline_sharpness = negative->BaselineSharpness();
                tuning_info.baseline_noise     = negative->BaselineNoise();
                
                const dng_linearization_info& linearization_info = *negative->GetLinearizationInfo();
                
                {
                    gpr_static_black_level& static_black_level    = tuning_info.static_black_level;

                    // Index into the black level pattern modulo its repeat dimensions. This
                    // expands a scalar (1x1) BlackLevel across all four CFA channels, while
                    // still reading a full 2x2 per-channel pattern when one is present.
                    const uint32 black_rows = linearization_info.fBlackLevelRepeatRows > 0 ? linearization_info.fBlackLevelRepeatRows : 1;
                    const uint32 black_cols = linearization_info.fBlackLevelRepeatCols > 0 ? linearization_info.fBlackLevelRepeatCols : 1;

                    static_black_level.r_black   = linearization_info.fBlackLevel[0 % black_rows][0 % black_cols][0];
                    static_black_level.g_r_black = linearization_info.fBlackLevel[0 % black_rows][1 % black_cols][0];
                    static_black_level.g_b_black = linearization_info.fBlackLevel[1 % black_rows][0 % black_cols][0];
                    static_black_level.b_black   = linearization_info.fBlackLevel[1 % black_rows][1 % black_cols][0];
                }
                
                {
                    gpr_saturation_level& dgain_saturation_level = tuning_info.dgain_saturation_level;
                    
                    dgain_saturation_level.level_red        = linearization_info.fWhiteLevel[0];
                    dgain_saturation_level.level_green_even = dgain_saturation_level.level_red;
                    dgain_saturation_level.level_green_odd  = dgain_saturation_level.level_red;
                    dgain_saturation_level.level_blue       = dgain_saturation_level.level_red;
                }
                
                {
                    dng_ifd &rawIFD = *info.fIFD [info.fMainIndex].Get ();

                    // Record how the (possibly larger) raw buffer relates to the final visible
                    // image, so callers can crop correctly without re-parsing the DNG themselves.
                    tuning_info.crop_info = parse_crop_info(rawIFD, negative);

                    // gpr/VC5 only supports single-channel Bayer CFA mosaics. Reject DNGs
                    // that store already-demosaiced/linear data (e.g. Apple ProRAW from the
                    // stock iOS Camera app, PhotometricInterpretation = LinearRaw) with a
                    // clear error instead of asserting deep in pixel-format detection below.
                    if( rawIFD.fPhotometricInterpretation != piCFA || rawIFD.fSamplesPerPixel != 1 )
                    {
                        fprintf( stderr, "Error: unsupported DNG -- expected a single-channel Bayer CFA raw image, "
                                          "but found PhotometricInterpretation=%u, SamplesPerPixel=%u.\n"
                                          "This usually means the DNG is already demosaiced/linear (e.g. Apple ProRAW "
                                          "from the stock iOS Camera app), which gpr_tools cannot encode/decode.\n",
                                          (unsigned)rawIFD.fPhotometricInterpretation, (unsigned)rawIFD.fSamplesPerPixel );
                        return false;
                    }

                    gpr_saturation_level& dgain_saturation_level = tuning_info.dgain_saturation_level;

                    bool rggb_raw = (rawIFD.fCFAPattern[0][0] == 0) && (rawIFD.fCFAPattern[0][1] == 1) && (rawIFD.fCFAPattern[1][0] == 1) && (rawIFD.fCFAPattern[1][1] == 2);

                    bool bggr_raw = (rawIFD.fCFAPattern[0][0] == 2) && (rawIFD.fCFAPattern[0][1] == 1) && (rawIFD.fCFAPattern[1][0] == 1) && (rawIFD.fCFAPattern[1][1] == 0);

                    const bool is_12bit = ( dgain_saturation_level.level_red        == 4095 &&
                                            dgain_saturation_level.level_green_even == 4095 &&
                                            dgain_saturation_level.level_green_odd  == 4095 &&
                                            dgain_saturation_level.level_blue       == 4095 );

                    const bool is_14bit = ( dgain_saturation_level.level_red        == 16383 &&
                                            dgain_saturation_level.level_green_even == 16383 &&
                                            dgain_saturation_level.level_green_odd  == 16383 &&
                                            dgain_saturation_level.level_blue       == 16383 );

                    if( rggb_raw )
                    {
                        if( is_12bit )
                            tuning_info.pixel_format = PIXEL_FORMAT_RGGB_12;
                        else
                            tuning_info.pixel_format = PIXEL_FORMAT_RGGB_14;
                    }
                    else if( bggr_raw )
                    {
                        if( is_12bit )
                            tuning_info.pixel_format = PIXEL_FORMAT_BGGR_12;
                        else
                            tuning_info.pixel_format = PIXEL_FORMAT_BGGR_14;
                    }
                    else
                    {
                        if( is_12bit )
                            tuning_info.pixel_format = PIXEL_FORMAT_GBRG_12;
                        else
                        {
                            assert(0);
                            return false;
                        }
                    }
                }
                
                // Noise profile
                if ( negative->HasNoiseProfile() )
                {
                    dng_noise_profile  noise_profile = negative->NoiseProfile();
                    
		            dng_noise_function  noise_function = noise_profile.NoiseFunction (0);

                    tuning_info.noise_scale = noise_function.Scale();
                    tuning_info.noise_offset = noise_function.Offset();
                    //LogPrint( "Noise profile s = %f, o = %f ", tuning_info.noise_scale, tuning_info.noise_offset );
               }
                
                // GainMap
                dng_opcode_list &opcodelist2 =  negative->OpcodeList2 ();
                uint32_t count = opcodelist2.Count ();
                // Note: this code will have to get smarter if we ever have anything other than four GainMap tags in OpcodeList2
                if ( count == 4 && tuning_info.gain_map.size == 0 )
                {
                    char gainmap_buffer [4][MAX_BUF_SIZE];
                    
                    for ( int i = 0; i < 4; i++ )
                    {
                        // Get GainMap Opcode
                        dng_opcode &opcode = opcodelist2.Entry( i );
                
                        // generate stream data
                        dng_stream stream ( gainmap_buffer[i], MAX_BUF_SIZE );
                        stream.Put_uint32 ( 0x01040000 ); // version
                        stream.Put_uint32 ( 0x3 ); // flags
                        opcode.PutData( stream );
                
                        // Point to buffer
                        if( i == 0 )
                            tuning_info.gain_map.size = stream.Position();
                        
                        assert( tuning_info.gain_map.buffers[i] == NULL );
                        
                        tuning_info.gain_map.buffers[i] = (char*)allocator->Alloc( tuning_info.gain_map.size );
                        memcpy( tuning_info.gain_map.buffers[i], gainmap_buffer[i], tuning_info.gain_map.size );
                    }
                }
                else
                {
                    tuning_info.gain_map.size = 0;
                }
                    
                // WarpRectilinear
                dng_opcode_list &opcodelist3 =  negative->OpcodeList3 ();
                count = opcodelist3.Count ();
                // Note: this code will have to get smarter if we ever have anything other than one WarpRectilinear tag in OpcodeList3
                if ( count == 1 )
                {
                    read_warp_rectilinear_opcode( opcodelist3.Entry( 0 ), tuning_info.warp );
                }
            }
        }
        
        // If the DNG carries a valid crop (ActiveArea + DefaultCrop) smaller than the full raw
        // buffer -- e.g. some sensors such as iPhone include extra border pixels outside the
        // visible image -- extract only that region, so the resulting raw buffer (and a GPR
        // encoded from it) is at the actual visible resolution rather than the full sensor size.
        dng_rect crop_rect;
        bool     have_crop = false;
        {
            dng_ifd &rawIFD = *info.fIFD [info.fMainIndex].Get ();
            gpr_crop_info crop_info = parse_crop_info(rawIFD, negative);
            
            dng_rect full_bounds = raw_image.Bounds();

            dng_rect candidate(  crop_info.active_area_top  + (int32)crop_info.default_crop_origin_v,
                                 crop_info.active_area_left + (int32)crop_info.default_crop_origin_h,
                                 crop_info.active_area_top  + (int32)crop_info.default_crop_origin_v + (int32)crop_info.default_crop_size_v,
                                 crop_info.active_area_left + (int32)crop_info.default_crop_origin_h + (int32)crop_info.default_crop_size_h );

            if( crop_info.default_crop_size_h > 0 && crop_info.default_crop_size_v > 0 &&
                candidate.l >= full_bounds.l && candidate.t >= full_bounds.t &&
                candidate.r <= full_bounds.r && candidate.b <= full_bounds.b &&
                ( candidate.W() < full_bounds.W() || candidate.H() < full_bounds.H() ) )
            {
                crop_rect = candidate;
                have_crop = true;
            }

            // Only override input_width/height/pitch when a smaller crop was actually found --
            // otherwise leave them as already set above (the full raw buffer's dimensions).
            // crop_rect is (0,0,0,0) when have_crop is false, so this must not run unconditionally.
            if( convert_params && have_crop )
            {
                convert_params->input_width  = crop_rect.W();
                convert_params->input_height = crop_rect.H();
                convert_params->input_pitch  = convert_params->input_width * 2;
            }
        }


        // When the caller can take the decoded image directly (and no crop applies, so the
        // image is the full raw exactly as a flattened buffer would have been), hand over
        // ownership instead of flattening - this skips a full-frame pixel copy. After the
        // detach, negative->RawImage() must not be called again.
        if( out_decoded_image && stage1_read && have_crop == false )
        {
            out_decoded_image->Reset( static_cast<dng_stage1_negative*>( negative.Get() )->DetachStage1Image() );
        }
        else if( raw_image_buffer )
        {
            CopyRawImageToBuffer( raw_image, *raw_image_buffer, have_crop ? &crop_rect : NULL );
        }
    }

    return true;
}

static void write_dng(const gpr_allocator*          allocator,
                            dng_stream*             dng_write_stream,
                      const gpr_buffer_auto*        raw_image_buffer,
                            bool                    compress_raw_to_vc5,
                            gpr_buffer_auto*        vc5_image_buffer,
                      const gpr_parameters*   	    convert_params,
                            AutoPtr<dng_image>*     prebuilt_raw_image = NULL )
{
    gpr_profile_info* profile_info  = (gpr_profile_info *) &convert_params->profile_info;
    const gpr_exif_info*    exif_info     = &convert_params->exif_info;

    const bool vc5_dng = compress_raw_to_vc5 || vc5_image_buffer;
    
    int i,j;

    int activeWidth  = convert_params->input_width;
    int activeHeight = convert_params->input_height;

    int outputWidth  = activeWidth;
    int outputHeight = activeHeight;
    
    gDNGShowTimers = false;
    
    dng_memory_allocator memalloc(gDefaultDNGMemoryAllocator);
    
    dng_rect rect(outputHeight, outputWidth);
    dng_host host(&memalloc);
    
    host.SetSaveDNGVersion(dngVersion_SaveDefault);
    host.SetSaveLinearDNG(false);
    host.SetKeepOriginalFile(true);
    
    // A prebuilt image (decoded by read_dng and handed over without flattening) replaces both
    // the fresh allocation here and the CopyBufferToRawImage fill below. Callers only pass one
    // for uncompressed output; the vc5 encoder reads from the flat buffer, not the image.
    const bool use_prebuilt = prebuilt_raw_image != NULL &&
                              prebuilt_raw_image->Get() != NULL &&
                              vc5_dng == false;

    AutoPtr<dng_image> image;

    if( use_prebuilt )
    {
        image.Reset( prebuilt_raw_image->Release() );
    }
    else
    {
        image.Reset( new dng_simple_image(rect, 1, ttShort, memalloc) );
    }
    
    gpr_buffer_auto raw_allocated_buffer( allocator->Alloc, allocator->Free );

    gpr_buffer_auto normalized_buffer( allocator->Alloc, allocator->Free );
    bool black_normalized = false;

    if( raw_image_buffer == NULL && vc5_image_buffer )
    {
#if GPR_READING
        vc5_decoder_parameters vc5_decoder_params;

        vc5_decoder_parameters_set_default(&vc5_decoder_params);

        vc5_decoder_params.mem_alloc        = allocator->Alloc;
        vc5_decoder_params.mem_free         = allocator->Free;

        switch(convert_params->tuning_info.pixel_format)
        {
            case PIXEL_FORMAT_RGGB_12:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_RGGB_12;
                break;

            case PIXEL_FORMAT_RGGB_14:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_RGGB_14;
                break;

            case PIXEL_FORMAT_GBRG_12:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_GBRG_12;
                break;

            case PIXEL_FORMAT_BGGR_12:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_BGGR_12;
                break;

            case PIXEL_FORMAT_BGGR_14:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_BGGR_14;
                break;

            default:
                assert(0);
                return;
        };

        gpr_buffer vc5_image = { vc5_image_buffer->get_buffer(), vc5_image_buffer->get_size() };
        gpr_buffer raw_image = { raw_allocated_buffer.get_buffer(), raw_allocated_buffer.get_size()  };
        
        if( vc5_decoder_process( &vc5_decoder_params, &vc5_image, &raw_image, NULL ) != CODEC_ERROR_OKAY )
        {
            assert(0);
        }
        
        raw_allocated_buffer.set( raw_image.buffer, raw_image.size, true );
        
        raw_image_buffer = &raw_allocated_buffer;
#else
        return; // Since vc5 decoder is not enabled, we cannot decode this vc5 bitstream to create raw image
#endif // GPR_READING
    }
    
    unsigned int input_pitch = convert_params->input_pitch;
    
    if( ( convert_params->tuning_info.pixel_format == PIXEL_FORMAT_GBRG_12P ||
          convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_12P ) &&
          vc5_dng == false && use_prebuilt == false )
    {
        unpack_pixel_format( raw_image_buffer, convert_params, &raw_allocated_buffer );
        
        raw_image_buffer = &raw_allocated_buffer;
        
        input_pitch = convert_params->input_width * 2;
    }

    // The protune log curve applied during VC5 encoding assumes a near-zero black level
    // (as on GoPro sensors). For sensors with a significant black pedestal (e.g. iPhone,
    // black level 528) the curve spends most of its codes on the sub-black region, starving
    // the real signal of precision and crushing mid-tone detail once a DNG reader expands it.
    // Subtract the black level and stretch to the full range before encoding so the curve's
    // precision lands on the actual signal; BlackLevel is then written as 0 below so any DNG
    // reader (including Lightroom, which uses its own decoder) reconstructs correct linear
    // values. This only affects the VC5/GPR path; uncompressed DNG output is unchanged.
    if( vc5_dng )
    {
        const gpr_static_black_level& sbl = convert_params->tuning_info.static_black_level;
        int black = ( sbl.r_black + sbl.g_r_black + sbl.g_b_black + sbl.b_black ) / 4;
        int white = convert_params->tuning_info.dgain_saturation_level.level_red;

        if( black > 0 && white > black )
        {
            size_t count = raw_image_buffer->get_size() / sizeof(uint16_t);
            normalized_buffer.allocate( raw_image_buffer->get_size() );

            const uint16_t* src = raw_image_buffer->to_uint16_t();
            uint16_t*       dst = normalized_buffer.to_uint16_t();

            const int range = white - black;
            for( size_t i = 0; i < count; i++ )
            {
                int v = (int)src[i] - black;
                if( v < 0 ) v = 0;
                v = (int)( (int64_t)v * white / range );
                if( v > white ) v = white;
                dst[i] = (uint16_t)v;
            }

            raw_image_buffer = &normalized_buffer;
            black_normalized = true;
        }
    }

    if( vc5_dng == false && use_prebuilt == false )
    {
        CopyBufferToRawImage( *raw_image_buffer, input_pitch / sizeof(short), *(image.Get()) );
    }

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    
    negative->SetOriginalBestQualityFinalSize( dng_point(outputHeight, outputWidth) );

    negative->SetOriginalDefaultFinalSize( dng_point(outputHeight, outputWidth) );
    
    { // Set Tuning Info
        const gpr_tuning_info*  tuning_info       = &convert_params->tuning_info;

        gpr_static_black_level static_black_level    = tuning_info->static_black_level;

        // The raw was black-subtracted before encoding (see above), so the stored data
        // already has a zero pedestal -- record that in the DNG.
        if( black_normalized )
        {
            static_black_level.r_black   = 0;
            static_black_level.g_r_black = 0;
            static_black_level.g_b_black = 0;
            static_black_level.b_black   = 0;
        }

        switch( convert_params->tuning_info.pixel_format )
        {
            case PIXEL_FORMAT_RGGB_12:
            case PIXEL_FORMAT_RGGB_12P:
            case PIXEL_FORMAT_RGGB_14:
                negative->SetQuadBlacks(static_black_level.r_black,
                                        static_black_level.g_r_black,
                                        static_black_level.g_b_black,
                                        static_black_level.b_black,
                                        -1 );
                break;
            case PIXEL_FORMAT_GBRG_12:
            case PIXEL_FORMAT_GBRG_12P:
                negative->SetQuadBlacks(static_black_level.g_b_black,
                                        static_black_level.b_black,
                                        static_black_level.r_black,
                                        static_black_level.g_r_black,
                                        -1 );
                break;

            case PIXEL_FORMAT_BGGR_12:
            case PIXEL_FORMAT_BGGR_14:
                negative->SetQuadBlacks(static_black_level.b_black,
                                        static_black_level.g_b_black,
                                        static_black_level.g_r_black,
                                        static_black_level.r_black,
                                        -1 );
                break;

            default:
                assert(0);
        }
        
        const gpr_saturation_level dgain_saturation_level = tuning_info->dgain_saturation_level;

        if( dgain_saturation_level.level_red == dgain_saturation_level.level_green_even &&
            dgain_saturation_level.level_red == dgain_saturation_level.level_green_odd &&
            dgain_saturation_level.level_red == dgain_saturation_level.level_blue )
        {
            negative->SetWhiteLevel( dgain_saturation_level.level_red, -1 );
        }
        else
        {
            negative->SetWhiteLevel( dgain_saturation_level.level_red, 0 );
            negative->SetWhiteLevel( dgain_saturation_level.level_green_even, 1 );
            negative->SetWhiteLevel( dgain_saturation_level.level_green_odd, 2 );
            negative->SetWhiteLevel( dgain_saturation_level.level_blue, 3 );
        }

        negative->SetBaseOrientation((dng_orientation::AdobeToDNG(tuning_info->orientation)));
        
        dng_vector camNeutral(3);
        
        camNeutral[0] = 1.0 / tuning_info->wb_gains.r_gain;
        camNeutral[1] = 1.0 / tuning_info->wb_gains.g_gain;
        camNeutral[2] = 1.0 / tuning_info->wb_gains.b_gain;
        
        negative->SetCameraNeutral(camNeutral);


        // Add noise profile
        if ( tuning_info->noise_scale > 0 )
        {
            std::vector<dng_noise_function> noiseFunctions;
            noiseFunctions.push_back( dng_noise_function ( tuning_info->noise_scale, tuning_info->noise_offset ) );
            negative->SetNoiseProfile( dng_noise_profile ( noiseFunctions ) );
        }
        
        size_t gain_map_size = tuning_info->gain_map.size;
        
        // GainMap - aka vignette or shading correction (one for each CFA channel)(applied before demosaicking (OpcodeList2))
        if ( gain_map_size > 0 && tuning_info->gain_map.buffers[0] != 0 && tuning_info->gain_map.buffers[1] != 0 && tuning_info->gain_map.buffers[2] != 0 && tuning_info->gain_map.buffers[3] != 0 )
        {
            dng_opcode_list &opcodelist2 =  negative->OpcodeList2 ();

            // Apple ImageIO/CIRAWFilter ignores the whole OpcodeList2 gain map unless the
            // two green CFA planes carry byte-identical gains ("Gain Map will be ignored
            // because of mismatch in green channels"); GoPro's factory maps differ between
            // the greens only by float16 quantization noise (<0.4%). Write the first
            // green's gain values into both green opcodes. Each opcode keeps its own area
            // spec (CFA plane origin); only the gain samples are shared. Done at DNG-write
            // time only, so the GPR-side buffers round-trip untouched.
            //
            // Which two of the four are the greens comes from each opcode's own area spec -
            // the (top,left) parity names its cell in the 2x2 CFA tile - never from its
            // position in the list: the buffers are stored in whatever order the source
            // file's OpcodeList2 used, which need not be CFA raster order. HERO13 GBRG
            // files store them R,G,G,B, so an index guess picks red and blue instead,
            // overwriting blue's shading with red's and leaving the greens mismatched -
            // Apple then drops all four maps and the corners stay ~36% dark.
            //
            // Greens sit on the CFA tile's main diagonal for patterns that begin with
            // green, on the anti-diagonal for the rest.
            bool greens_on_main_diagonal;
            switch ( tuning_info->pixel_format )
            {
                case PIXEL_FORMAT_GBRG_12:      // G B / R G
                case PIXEL_FORMAT_GBRG_12P:
                    greens_on_main_diagonal = true;
                    break;
                default:                        // RGGB: R G / G B,  BGGR: B G / G R
                    greens_on_main_diagonal = false;
                    break;
            }

            int greens[4];
            int green_count = 0;

            for ( int i = 0; i < 4; i++ )
            {
                // Read the area spec back through dng_stream - the same reader that turns
                // these buffers into opcodes below - so the byte order matches whatever
                // the writing side used, by construction.
                dng_stream area_stream ( tuning_info->gain_map.buffers[i], gain_map_size );

                area_stream.Get_uint32();       // version
                area_stream.Get_uint32();       // flags
                area_stream.Get_uint32();       // opcode data size

                const int32 area_top  = area_stream.Get_int32();
                const int32 area_left = area_stream.Get_int32();

                if ( ( ( area_top & 1 ) == ( area_left & 1 ) ) == greens_on_main_diagonal )
                {
                    greens[green_count++] = i;
                }
            }

            // Serialized buffer layout (mirrors the read code above): version(4) + flags(4)
            // + opcode data size(4) + dng_area_spec(32) + gain map header(44); the gain
            // samples run from there to the end of the buffer.
            const size_t gain_data_offset = 4 + 4 + 4 + dng_area_spec::kDataSize + 44;
            const size_t map_header_offset = gain_data_offset - 44;

            // Only share gains when the area specs named exactly two greens and both
            // describe the same map geometry (points, spacing, origin, planes); otherwise
            // the sample counts could differ. Anything unexpected writes the four maps
            // through untouched rather than guessing at them.
            const bool share_green_gains =
                green_count == 2 &&
                gain_map_size > gain_data_offset &&
                memcmp( tuning_info->gain_map.buffers[greens[0]] + map_header_offset,
                        tuning_info->gain_map.buffers[greens[1]] + map_header_offset, 44 ) == 0;

            const int green_b = share_green_gains ? greens[1] : -1;

            std::vector<char> green_b_buffer;

            if ( share_green_gains )
            {
                green_b_buffer.assign( tuning_info->gain_map.buffers[green_b],
                                       tuning_info->gain_map.buffers[green_b] + gain_map_size );

                memcpy( &green_b_buffer[gain_data_offset],
                        tuning_info->gain_map.buffers[greens[0]] + gain_data_offset,
                        gain_map_size - gain_data_offset );
            }

            for ( int i = 0; i < 4; i++ )
            {
                const char* buffer = ( i == green_b ) ? &green_b_buffer[0] : tuning_info->gain_map.buffers[i];

                dng_stream gain_map_stream ( buffer, gain_map_size );
                AutoPtr<dng_opcode> gain_map_opcode ( new dng_opcode_GainMap ( host, gain_map_stream ));
                opcodelist2.Append( gain_map_opcode );
            }
        }

        // WarpRectilinear (OpcodeList3, applied after demosaicking): chromatic aberration
        // and/or geometric lens distortion correction, written at full fidelity from tuning_info
        if ( gpr_warp_rectilinear_is_valid( &tuning_info->warp ) )
        {
            const gpr_warp_rectilinear &warp = tuning_info->warp;

            dng_warp_params_rectilinear warp_params;

            warp_params.fPlanes = warp.planes;
            warp_params.fCenter = dng_point_real64( warp.center_y, warp.center_x );

            for ( uint32 p = 0; p < warp.planes; p++ )
            {
                for ( int i = 0; i < 4; i++ )
                    warp_params.fRadParams[p][i] = warp.radial[p][i];

                warp_params.fTanParams[p][0] = warp.tangential[p][0];
                warp_params.fTanParams[p][1] = warp.tangential[p][1];
            }

            if ( warp_params.IsValid() )
            {
                AutoPtr<dng_opcode> warp_opcode ( new dng_opcode_WarpRectilinear ( warp_params, warp.flags ));

                negative->OpcodeList3 ().Append( warp_opcode );
            }
        }
    }
    
    //GP!! NEED outputWidth, activeWidth, outputHeight, activeHeight here
    negative->SetDefaultScale(dng_urational(outputWidth, activeWidth), dng_urational(outputHeight, activeHeight));

    {
        dng_point crop_origin( 0, 0 );
        dng_point crop_size( activeHeight, activeWidth );

        negative->SetDefaultCropOrigin( crop_origin.h, crop_origin.v );
        negative->SetDefaultCropSize( crop_size.h, crop_size.v );

        negative->SetOriginalDefaultCropSize( dng_urational(crop_size.h, 1), dng_urational(crop_size.v, 1) );

        dng_rect activeArea = dng_rect(activeHeight, activeWidth);

        negative->SetActiveArea(activeArea);
    }
    
    char camera_make_and_model[256];
    
    gpr_exif_info_get_camera_make_and_model(exif_info, camera_make_and_model);

    negative->SetModelName( camera_make_and_model );

    negative->SetLocalName( camera_make_and_model );
    
    //GP!! "Raw file?"
    negative->SetOriginalRawFileName("RAW FILE");
    //GP!! Either 1 or 3
    negative->SetColorChannels(3);
    
    //!! Need correct CFA sequence here!!
    ColorKeyCode colorCodes[4] = { colorKeyRed, colorKeyGreen, colorKeyBlue, colorKeyGreen };
    //  ColorKeyCode colorCodes[4] = { colorKeyGreen, colorKeyRed, colorKeyGreen, colorKeyBlue };
    
    negative->SetColorKeys(colorCodes[0], colorCodes[1], colorCodes[2], colorCodes[3]);

    // Set Bayer Pattern
    if( convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_12 || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_12P || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_14 )
    {
        negative->SetBayerMosaic(1);
    }
    else if( convert_params->tuning_info.pixel_format == PIXEL_FORMAT_GBRG_12 || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_GBRG_12P )
    {
        negative->SetBayerMosaic(3);
    }
    else if( convert_params->tuning_info.pixel_format == PIXEL_FORMAT_BGGR_12 || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_BGGR_14 )
    {
        negative->SetBayerMosaic(2);
    }
    else
    {
        assert(0);
        return;
    }
    
    negative->SetBaselineExposure(convert_params->tuning_info.baseline_exposure);
    negative->SetBaselineNoise(convert_params->tuning_info.baseline_noise);
    negative->SetBaselineSharpness(convert_params->tuning_info.baseline_sharpness);
    
    negative->SetAntiAliasStrength(dng_urational(100, 100));
    negative->SetLinearResponseLimit(1.0);
    negative->SetShadowScale( dng_urational(1, 1) );
    negative->SetAnalogBalance(dng_vector_3(1.0, 1.0, 1.0));
    
    AutoPtr<dng_camera_profile> prof(new dng_camera_profile);
    prof->SetName( camera_make_and_model );
    
    dng_matrix_3by3 mColor1;
    dng_matrix_3by3 mColor2;
    
    if ( profile_info->compute_color_matrix )
    {
        double matrix_weighting = profile_info->matrix_weighting;
        
        double out_matrix[3][3];
    
        if ( matrix_weighting < 0.0 || matrix_weighting > 1.0 )
            matrix_weighting = 1.0;
    
        compute_xyz_to_camera_color_matrix( profile_info->cam_to_srgb_1, profile_info->wb1, matrix_weighting, out_matrix );
    
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                mColor1[i][j] = out_matrix[i][j];
    
        compute_xyz_to_camera_color_matrix( profile_info->cam_to_srgb_2, profile_info->wb2, matrix_weighting, out_matrix );
    
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                mColor2[i][j] = out_matrix[i][j];
        }
    else
    {
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
            {
                mColor1[i][j] = profile_info->color_matrix_1[i][j];
                mColor2[i][j] = profile_info->color_matrix_2[i][j];
            }
    }
   
    prof->SetColorMatrix1((dng_matrix) mColor1);
    prof->SetColorMatrix2((dng_matrix) mColor2);
    
    prof->SetCalibrationIlluminant1(profile_info->illuminant1);
    prof->SetCalibrationIlluminant2(profile_info->illuminant2);
    
    negative->AddProfile(prof);
    
    dng_exif* const exif = negative->GetExif();
    exif->fModel.Set( exif_info->camera_model );
    exif->fMake.Set( exif_info->camera_make );
    
    convert_dng_exif_info_to_dng_exif(exif, exif_info );
    
    {
        dng_date_time_info date_time_original;
        date_time_original.SetDateTime( convert_to_dng_date_time( exif_info->date_time_original ) );
        
        negative->UpdateDateTime(date_time_original);
    }
    
    if( convert_params->gpmf_payload.buffer != NULL && convert_params->gpmf_payload.size > 0 )
    { // Set GPMF Data
        dng_string gopro_tag;
        gopro_tag.Append("GoPro\n");
        
        AutoPtr<dng_memory_block> gpmf_buffer;
        
        gpmf_buffer.Reset (host.Allocate(gopro_tag.Length() + convert_params->gpmf_payload.size) );
        
        unsigned char* gpmf_buffer_ptr = (unsigned char*)gpmf_buffer.Get()->Buffer();
        
        memcpy( gpmf_buffer_ptr + 0, gopro_tag.Get(), gopro_tag.Length() );
        memcpy( gpmf_buffer_ptr + gopro_tag.Length(), convert_params->gpmf_payload.buffer, convert_params->gpmf_payload.size );
        
        negative->SetPrivateData(gpmf_buffer);
    }    
  
    negative->SetStage1Image(image);
    
    dng_preview_list* preview_list = NULL;
  
    dng_image_writer* writer = NULL;
  
#if GPR_WRITING
    gpr_image_writer* gpr_writer = NULL;

    if( vc5_dng )
    {
        gpr_writer = new gpr_image_writer(raw_image_buffer, convert_params->input_width, convert_params->input_height, convert_params->input_pitch, vc5_image_buffer );

        // The preview is generated from the (already black-subtracted) encoded data, so its
        // black level must be 0 too -- otherwise it would be subtracted a second time.
        gpr_parameters enc_params = *convert_params;
        if( black_normalized )
        {
            enc_params.tuning_info.static_black_level.r_black   = 0;
            enc_params.tuning_info.static_black_level.g_r_black = 0;
            enc_params.tuning_info.static_black_level.g_b_black = 0;
            enc_params.tuning_info.static_black_level.b_black   = 0;
        }
        // Outlives EncodeVc5Image deliberately: it owns the gain samples the preview
        // render reads through rgb_params.shading_map.
        rgb_shading_map_tables preview_shading_tables;

        set_vc5_encoder_parameters( gpr_writer->GetVc5EncoderParams(), &enc_params, preview_shading_tables );

        gpr_writer->EncodeVc5Image();

        writer = gpr_writer;
    }
    else
#endif
    {
        writer = new dng_image_writer;
    }

    // Preview / thumbnail, for both output paths. Without one, WriteDNG leaves the raw image
    // itself in IFD 0, where the DNG spec says the thumbnail lives - so a reader that shows
    // IFD 0 (Finder, Lightroom, Bridge, anything that does not run its own raw pipeline first)
    // renders undemosaiced 12-bit CFA samples in a 16-bit container, and the file looks black.
    // Only the GPR path can generate one for itself, as a by-product of the VC5 encode; the
    // plain-DNG path runs no encoder, so its preview arrives in preview_image.
    if( convert_params->enable_preview )
    {
        const gpr_preview_image& preview_image = convert_params->preview_image;

        // Read the preview's pixel dimensions straight from the JPEG header (no decode
        // needed), so callers only have to supply the compressed JPEG bytes. A JPEG whose
        // header will not parse is dropped rather than embedded as a 0x0 preview, which
        // makes the DNG writer build a preview IFD with zero tiles (a crash, historically).
        int preview_w = 0, preview_h = 0;

        if( preview_image.jpg_preview.size > 0 && preview_image.jpg_preview.buffer != NULL &&
            gpr_jpeg_get_dimensions( (const unsigned char*)preview_image.jpg_preview.buffer,
                                     preview_image.jpg_preview.size, &preview_w, &preview_h ) )
        {
            add_preview_to_list(host, preview_list, preview_image.jpg_preview.buffer, preview_h, preview_w, preview_image.jpg_preview.size );
        }
#if GPR_WRITING && GPR_JPEG_AVAILABLE
        // Only embed the auto-generated thumbnail when one actually exists. It is produced as
        // a side effect of EncodeVc5Image(); when the vc5 bitstream is supplied pre-encoded
        // (e.g. gpr_convert_vc5_to_gpr / vc5_to_dng) encoding is skipped and the thumbnail is
        // empty.
        else if( gpr_writer != NULL &&
                 gpr_writer->get_rgb_thumbnail().buffer != NULL &&
                 gpr_writer->get_rgb_thumbnail().width  > 0 &&
                 gpr_writer->get_rgb_thumbnail().height > 0 )
        {
            const gpr_rgb_buffer& rgb_buffer = gpr_writer->get_rgb_thumbnail();

            // The sink (shared with gpr_encode_rgb_to_jpg) grows its buffer to fit whatever
            // tiny_jpeg produces; sink.data is libc-heap owned and freed below.
            gpr_jpg_sink sink = { NULL, 0, 0, false };

            int encoded = tje_encode_with_func(gpr_jpg_sink_write, (void*)&sink, 2, rgb_buffer.width, rgb_buffer.height, 3, (const unsigned char*)rgb_buffer.buffer );

            // A thumbnail that failed to encode (or to fit in memory) is dropped rather than
            // embedded truncated; the GPR is valid without a preview.
            if( encoded && sink.failed == false && sink.size > 0 )
            {
                add_preview_to_list(host, preview_list, sink.data, rgb_buffer.height, rgb_buffer.width, sink.size );

                // Free up sink
                free( sink.data );
            }
            else
            {
                LogPrint("Failed to load preview from GPR encoded images");
            }
        }
#endif
    }

    writer->SetComputeMd5Sum( convert_params->compute_md5sum );
    
    assert(writer);
    writer->WriteDNG(host, *dng_write_stream, *negative.Get(), preview_list, dngVersion_Current, vc5_dng == false );
  
    delete writer;
  
  if( preview_list )
    delete preview_list;
}

extern dng_memory_allocator gDefaultDNGMemoryAllocator;

bool write_dngstream_to_buffer( dng_stream* stream, gpr_buffer* output_buffer, gpr_malloc mem_alloc, gpr_free mem_free )
{
    size_t buffer_size  = stream->Length();
    void*  buffer       = mem_alloc(buffer_size);
    
    stream->SetReadPosition(0);
    stream->Get(buffer, buffer_size);
    
    output_buffer->buffer = buffer;
    output_buffer->size   = buffer_size;
    
    return true;
}

bool gpr_parameters_parse_dng(const gpr_allocator*  allocator,
                                    gpr_buffer*     inp_dng_buffer,
                                    gpr_parameters* parameters)
{
    try
    {
        // Read-only view over the caller's buffer - no copy of the input file
        dng_stream inp_dng_stream( inp_dng_buffer->buffer, (uint32)inp_dng_buffer->size );

        return read_dng( allocator, &inp_dng_stream, NULL, NULL, parameters );
    }
    catch( ... ) // the DNG SDK throws dng_exception on malformed input; C callers expect false
    {
        return false;
    }
}

bool gpr_parameters_parse_dng_file(const gpr_allocator*  allocator,
                                   const char*           inp_file_path,
                                         gpr_parameters* parameters)
{
    try
    {
        dng_file_stream inp_dng_stream( inp_file_path );

        return read_dng( allocator, &inp_dng_stream, NULL, NULL, parameters );
    }
    catch( ... ) // dng_file_stream and the DNG SDK throw dng_exception (unopenable or malformed file)
    {
        return false;
    }
}

bool gpr_parse_metadata(const gpr_allocator*        allocator,
                              gpr_buffer*           inp_dng_buffer,
                              gpr_parameters*       parameters)
{
    return gpr_parameters_parse_dng( allocator, inp_dng_buffer, parameters );
}

// Shift the start of a raw image by input_skip_rows/cols to adjust its Bayer phase
// (e.g. BGGR -> GBRG). The historical implementation returned a pointer alias into
// raw_buffer, which made the encoder read the last skipped rows/columns from just past
// the image end - an out-of-bounds read that segfaults once the allocation is large
// enough to be page-exact. Instead, build a full-size shifted copy: the tail (the frame's
// outermost bottom/right edge, which historically held whatever garbage followed the
// allocation) is filled with the source's own last bytes, keeping values in sensor range.
static const gpr_buffer_auto* adjust_bayer_phase( const gpr_parameters* parameters,
                                                  const gpr_buffer_auto* raw_buffer,
                                                        gpr_buffer_auto* shifted_copy )
{
    const size_t phase_offset = (size_t)parameters->input_skip_rows * parameters->input_pitch
                              + (size_t)parameters->input_skip_cols * sizeof(uint16_t);

    const size_t size = raw_buffer->get_size();

    if( phase_offset == 0 || phase_offset >= size )
        return raw_buffer;

    shifted_copy->allocate( size );

    const char* src = (const char*)raw_buffer->get_buffer();
    char*       dst = (char*)shifted_copy->get_buffer();

    memcpy( dst, src + phase_offset, size - phase_offset );
    memcpy( dst + size - phase_offset, src + size - phase_offset, phase_offset );

    return shifted_copy;
}

bool gpr_convert_raw_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_raw_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 2)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    raw_buffer.set( (char*)inp_raw_buffer->buffer, inp_raw_buffer->size );

    gpr_buffer_auto shifted_copy(allocator->Alloc, allocator->Free);

    dng_memory_stream out_dng_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_dng_stream, adjust_bayer_phase( parameters, &raw_buffer, &shifted_copy ), false, NULL, parameters );

    write_dngstream_to_buffer( &out_dng_stream, out_dng_buffer, allocator->Alloc, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_dng_to_raw(const gpr_allocator*    allocator,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_raw_buffer)
{
    TIMESTAMP("[BEG]", 2)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    // Read-only view over the caller's buffer - no copy of the input file
    dng_stream inp_dng_stream( inp_dng_buffer->buffer, (uint32)inp_dng_buffer->size );

    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL, NULL, NULL ) == false )
    {
        assert(0); return false;
    }

    out_raw_buffer->buffer = allocator->Alloc( raw_buffer.get_size() );
    out_raw_buffer->size = raw_buffer.get_size();
    
    memcpy(out_raw_buffer->buffer, raw_buffer.get_buffer(), raw_buffer.get_size() );
    
    TIMESTAMP("[END]", 1)
    
    return true;
}

//!< dng to dng conversion
bool gpr_convert_dng_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);

    // Read-only view over the caller's buffer - no copy of the input file
    dng_stream inp_dng_stream( inp_dng_buffer->buffer, (uint32)inp_dng_buffer->size );

    // Fast path: hand the decoded image straight to write_dng, skipping the flatten/refill
    // pixel copies. Only possible when the flat buffer isn't needed along the way: no Bayer
    // phase shift (which is pointer arithmetic on the flat buffer) and no 12P repacking.
    const bool want_fast = parameters->input_skip_rows == 0 &&
                           parameters->input_skip_cols == 0 &&
                           parameters->tuning_info.pixel_format != PIXEL_FORMAT_RGGB_12P &&
                           parameters->tuning_info.pixel_format != PIXEL_FORMAT_GBRG_12P;

    AutoPtr<dng_image> decoded_image;

    // Carry the input's thumbnail across: this rewrite exists to change metadata, and dropping
    // the preview would change how every reader shows the file (see write_dng). Nothing is
    // decoded or re-encoded - the JPEG the input already carries is copied through.
    gpr_buffer_auto preview_jpg(allocator->Alloc, allocator->Free);
    gpr_parameters  preview_params;

    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL, NULL, NULL,
                  want_fast ? &decoded_image : NULL, &preview_jpg ) == false )
    {
        assert(0); return false;
    }

    parameters = attach_preview( parameters, &preview_params, &preview_jpg );

    // The decoded image must match what write_dng would have built from the flat buffer;
    // read_dng already withholds it when a crop applies, so a mismatch here only means the
    // caller's parameters disagree with the file - rebuild the legacy flat buffer and fall back.
    const bool fast = decoded_image.Get() != NULL &&
                      decoded_image->Planes() == 1 &&
                      decoded_image->PixelType() == ttShort &&
                      decoded_image->Bounds() == dng_rect( parameters->input_height, parameters->input_width );

    if( fast == false && decoded_image.Get() != NULL )
    {
        CopyRawImageToBuffer( *decoded_image.Get(), raw_buffer, NULL );
        decoded_image.Reset();
    }

    gpr_buffer_auto shifted_copy(allocator->Alloc, allocator->Free);

    // Output of a metadata rewrite is about the size of the input, so reserving input size
    // plus slack means the stream essentially never regrows; detach() then hands the buffer
    // to the caller with no final copy.
    gpr_flat_write_stream out_dng_stream( allocator->Alloc, allocator->Free, (uint64)inp_dng_buffer->size + (1 << 20) );

    if( fast )
    {
        write_dng( allocator, &out_dng_stream, NULL, false, NULL, parameters, &decoded_image );
    }
    else
    {
        write_dng( allocator, &out_dng_stream, adjust_bayer_phase( parameters, &raw_buffer, &shifted_copy ), false, NULL, parameters );
    }

    out_dng_stream.detach( out_dng_buffer );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_vc5_to_gpr(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_vc5_buffer,
                                  gpr_buffer*       out_gpr_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    vc5_buffer.set( (char*)inp_vc5_buffer->buffer, inp_vc5_buffer->size );
    
    dng_memory_stream out_gpr_stream( gDefaultDNGMemoryAllocator );
    
    write_dng( allocator, &out_gpr_stream, NULL, false, &vc5_buffer, parameters );

    write_dngstream_to_buffer( &out_gpr_stream, out_gpr_buffer, allocator->Alloc, allocator->Free );
    
    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_gpr_to_vc5(const gpr_allocator*            allocator,
                                  gpr_buffer*               inp_gpr_buffer,
                                  gpr_buffer*               out_vc5_buffer)
{
    TIMESTAMP("[BEG]", 2)
    
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    
    // Read-only view over the caller's buffer - no copy of the input file
    dng_stream inp_gpr_stream( inp_gpr_buffer->buffer, (uint32)inp_gpr_buffer->size );
    

    if( read_dng( allocator, &inp_gpr_stream, NULL, &vc5_buffer ) == false )
    {
        assert(0); return false;
    }
    
    if( vc5_buffer.is_valid() == false )
    {
        return false;
    }
    
    out_vc5_buffer->buffer = allocator->Alloc( vc5_buffer.get_size() );
    out_vc5_buffer->size = vc5_buffer.get_size();
    memcpy(out_vc5_buffer->buffer, vc5_buffer.get_buffer(), vc5_buffer.get_size() );

    TIMESTAMP("[END]", 1)
    
    return true;
}

#if GPR_WRITING

bool gpr_convert_raw_to_gpr(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_raw_buffer,
                                  gpr_buffer*       out_gpr_buffer)
{
    TIMESTAMP("[BEG]", 2)

    if( quality_is_valid( parameters ) == false )
        return false;
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);

    raw_buffer.set(inp_raw_buffer->buffer, inp_raw_buffer->size);

    gpr_buffer_auto shifted_copy(allocator->Alloc, allocator->Free);

    dng_memory_stream out_gpr_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_gpr_stream, adjust_bayer_phase( parameters, &raw_buffer, &shifted_copy ), true, NULL, parameters );

    write_dngstream_to_buffer( &out_gpr_stream, out_gpr_buffer, allocator->Alloc, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_dng_to_gpr(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_gpr_buffer)
{
    TIMESTAMP("[BEG]", 2)

    if( quality_is_valid( parameters ) == false )
        return false;

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    // Read-only view over the caller's buffer - no copy of the input file
    dng_stream inp_dng_stream( inp_dng_buffer->buffer, (uint32)inp_dng_buffer->size );
    
    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL, NULL, NULL ) == false )
    {
        assert(0); return false;
    }

    gpr_buffer_auto shifted_copy(allocator->Alloc, allocator->Free);

    dng_memory_stream out_gpr_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_gpr_stream, adjust_bayer_phase( parameters, &raw_buffer, &shifted_copy ), true, NULL, parameters );
    
    write_dngstream_to_buffer( &out_gpr_stream, out_gpr_buffer, allocator->Alloc, allocator->Free );
    
    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_dng_to_vc5(const gpr_allocator*    allocator,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_vc5_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    gpr_parameters  params;
    bool            is_vc5_format = false;

    gpr_parameters_set_defaults( &params );

    {
        // Read-only view over the caller's buffer - no copy of the input file
        dng_stream inp_dng_stream( inp_dng_buffer->buffer, (uint32)inp_dng_buffer->size );

        // Decode the raw image and read the metadata; also extract the vc5 bitstream if the
        // input DNG is already vc5-compressed (i.e. a GPR).
        if( read_dng( allocator, &inp_dng_stream, &raw_buffer, &vc5_buffer, &params, &is_vc5_format ) == false )
        {
            assert(0);
            gpr_parameters_destroy( &params, allocator->Free );
            return false;
        }
    }

    bool ok = true;

    if( is_vc5_format && vc5_buffer.is_valid() )
    {
        // Input already carried a vc5 bitstream -- return it directly.
        out_vc5_buffer->buffer = allocator->Alloc( vc5_buffer.get_size() );
        out_vc5_buffer->size   = vc5_buffer.get_size();
        memcpy( out_vc5_buffer->buffer, vc5_buffer.get_buffer(), vc5_buffer.get_size() );
    }
    else
    {
        // Uncompressed DNG: encode the decoded raw to a GPR, then extract its vc5 bitstream.
        // (The previous implementation never ran the encoder and returned an empty buffer.)
        gpr_buffer raw_image = { raw_buffer.get_buffer(), raw_buffer.get_size() };
        gpr_buffer gpr_image = { NULL, 0 };

        ok = gpr_convert_raw_to_gpr( allocator, &params, &raw_image, &gpr_image );

        if( ok )
            ok = gpr_convert_gpr_to_vc5( allocator, &gpr_image, out_vc5_buffer );

        if( gpr_image.buffer )
            allocator->Free( gpr_image.buffer );
    }

    gpr_parameters_destroy( &params, allocator->Free );

    TIMESTAMP("[END]", 1)

    return ok;
}

#endif // GPR_WRITING

#if GPR_WRITING && GPR_READING
// Repackages the input's vc5 bitstream in a new GPR container with the caller's metadata,
// avoiding the vc5 decode/re-encode entirely. The auto-generated thumbnail is a by-product
// of running the vc5 encoder, so when the caller requests a preview without supplying the
// JPEG bytes (enable_preview set, preview_image empty) the input is decoded and re-encoded
// from scratch instead, as it is when the caller asks for that outright (reencode).
bool gpr_convert_gpr_to_gpr(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_gpr_buffer,
                                  gpr_buffer*       out_gpr_buffer)
{
    TIMESTAMP("[BEG]", 2)

    bool needs_encoded_thumbnail = false;

#if GPR_JPEG_AVAILABLE
    needs_encoded_thumbnail = parameters->enable_preview &&
        parameters->preview_resolution != GPR_RGB_RESOLUTION_NONE &&
        ( parameters->preview_image.jpg_preview.buffer == NULL ||
          parameters->preview_image.jpg_preview.size == 0 );
#endif

    const bool needs_encode = needs_encoded_thumbnail || parameters->reencode;

    if( needs_encode && quality_is_valid( parameters ) == false )
        return false;

    if( needs_encode == false )
    {
        gpr_buffer vc5_buffer = { NULL, 0 };

        if( gpr_convert_gpr_to_vc5( allocator, inp_gpr_buffer, &vc5_buffer ) )
        {
            bool ok = gpr_convert_vc5_to_gpr( allocator, parameters, &vc5_buffer, out_gpr_buffer );

            allocator->Free( vc5_buffer.buffer );

            TIMESTAMP("[END]", 1)

            return ok;
        }

        // No vc5 bitstream found (e.g. the input is an uncompressed DNG): fall through to
        // the decode + encode path below.
    }

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);

    // Read-only view over the caller's buffer - no copy of the input file
    dng_stream inp_gpr_stream( inp_gpr_buffer->buffer, (uint32)inp_gpr_buffer->size );

    if( read_dng( allocator, &inp_gpr_stream, &raw_buffer, NULL, NULL, NULL ) == false )
    {
        assert(0); return false;
    }

    dng_memory_stream out_gpr_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_gpr_stream, &raw_buffer, true, NULL, parameters );

    write_dngstream_to_buffer( &out_gpr_stream, out_gpr_buffer, allocator->Alloc, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}
#endif // GPR_WRITING && GPR_READING

#if GPR_READING

bool gpr_convert_gpr_to_rgb(const gpr_allocator*        allocator,
                                  GPR_RGB_RESOLUTION    rgb_resolution,
                                  int                   rgb_bits,
                                  gpr_buffer*           inp_gpr_buffer,
                                  gpr_rgb_buffer*       out_rgb_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_parameters params;

    gpr_parameters_set_defaults( &params );

    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);

    try
    {
        // Read-only view over the caller's buffer - no copy of the input file
        dng_stream inp_gpr_stream( inp_gpr_buffer->buffer, (uint32)inp_gpr_buffer->size );

        if( read_dng( allocator, &inp_gpr_stream, NULL, &vc5_buffer, &params ) == false )
        {
            gpr_parameters_destroy( &params, allocator->Free );
            return false;
        }
    }
    catch( ... ) // the DNG SDK throws dng_exception on malformed input; C callers expect false
    {
        gpr_parameters_destroy( &params, allocator->Free );
        return false;
    }

    if( vc5_buffer.is_valid() == false )
    {
        gpr_parameters_destroy( &params, allocator->Free );
        return false;
    }
    
    vc5_decoder_parameters vc5_decoder_params;

    vc5_decoder_parameters_set_default(&vc5_decoder_params);

    vc5_decoder_params.mem_alloc        = allocator->Alloc;
    vc5_decoder_params.mem_free         = allocator->Free;
    vc5_decoder_params.pixel_format     = VC5_DECODER_PIXEL_FORMAT_DEFAULT;

    RGB_PARAMETERS& rgb_params = vc5_decoder_params.rgb_params;
    rgb_params.bits = rgb_bits;

    gpr_rgb_gain&   rgb_gain = rgb_params.white_balance_gain;

    find_rational( params.tuning_info.wb_gains.r_gain, 0.125, &rgb_gain.r_gain_num, &rgb_gain.r_gain_pow2_den );
    find_rational( params.tuning_info.wb_gains.g_gain, 0.125, &rgb_gain.g_gain_num, &rgb_gain.g_gain_pow2_den );
    find_rational( params.tuning_info.wb_gains.b_gain, 0.125, &rgb_gain.b_gain_num, &rgb_gain.b_gain_pow2_den );

    // Camera -> sRGB color matrix from the DNG color profile, so the RGB output renders
    // accurate hues instead of treating white-balanced camera RGB as if it were sRGB
    // (see WaveletToRGB). Identity when the profile is missing or degenerate.
    compute_camera_to_srgb_color_matrix( &params.tuning_info, &params.profile_info, rgb_params.color_matrix );

    // Lens shading correction from the source's OpcodeList2 GainMap opcodes. Lives until
    // vc5_decoder_process returns, which is what reads through it.
    rgb_shading_map_tables shading_tables;

    // Baseline exposure from the DNG (EV), applied as linear gain ahead of the tone curve
    // like DNG renderers do; 0 when the source does not set it.
    //
    // A positive exposure is dropped when the source's gain maps carry real gain. The
    // camera calibrates BaselineExposure against a renderer whose highlight shoulder has
    // headroom this pipeline's tone curve does not: measured on MISSION1-50MP (+1.48 EV),
    // honoring it alongside the gains clips 14% of the frame where Apple's RAW pipeline,
    // reading the same tag and the same maps, clips none.
    //
    // The test is the gains themselves, not merely the presence of the opcodes: a camera
    // may ship four IDENTITY maps for a capture mode that needs no correction, and those
    // withhold nothing to compensate for. Keying on presence is what left MISSION 1 PRO
    // 12 MP files - whose four maps are all exactly 1.0 - rendering ~1 stop dark.
    //
    // A negative exposure only darkens and cannot clip, so it stays honored either way
    // (e.g. HERO5, -0.3 EV, which also carries gain maps).
    {
        const bool shading_applied = shading_tables.build( &params.tuning_info, &rgb_params );

        const bool maps_carry_gain = shading_applied ? ( shading_tables.max_gain() > 1.01f )
                                                     : params.tuning_info.has_opcode_gain_maps;

        float baseline_exposure = (float)params.tuning_info.baseline_exposure;

        if ( maps_carry_gain && baseline_exposure > 0.0f )
        {
            baseline_exposure = 0.0f;
        }

        rgb_params.baseline_exposure = baseline_exposure;
    }

    // Remove the sensor black pedestal before the gains are applied, otherwise it tints the RGB output.
    rgb_params.black_level = compute_rgb_black_level( &params.tuning_info );

    rgb_params.resolution = rgb_resolution;

    if( vc5_decoder_process( &vc5_decoder_params, &vc5_buffer.get_gpr_buffer(), NULL, out_rgb_buffer ) != CODEC_ERROR_OKAY )
    {
        gpr_parameters_destroy( &params, allocator->Free );
        return false;
    }

    gpr_parameters_destroy( &params, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_gpr_to_ppm(const gpr_allocator*        allocator,
                                  GPR_RGB_RESOLUTION    rgb_resolution,
                                  int                   rgb_bits,
                                  gpr_buffer*           inp_gpr_buffer,
                                  gpr_buffer*           out_ppm_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_rgb_buffer rgb_buffer = { NULL, 0, 0, 0 };

    if( gpr_convert_gpr_to_rgb( allocator, rgb_resolution, rgb_bits, inp_gpr_buffer, &rgb_buffer ) == false )
        return false;

    // Assemble the PPM: a short ASCII header ("P6 <width> <height> <maxval>") followed by the
    // interleaved RGB samples. PPM has no metadata channel, so orientation cannot be recorded here.
    char header[64];
    int  maxval     = ( rgb_bits == 8 ) ? 255 : 65535;
    int  header_len = snprintf( header, sizeof(header), "P6\n%lu %lu\n%d\n",
                                (unsigned long)rgb_buffer.width, (unsigned long)rgb_buffer.height, maxval );

    out_ppm_buffer->size   = rgb_buffer.size + (size_t)header_len;
    out_ppm_buffer->buffer = allocator->Alloc( out_ppm_buffer->size );

    if( out_ppm_buffer->buffer == NULL )
    {
        allocator->Free( rgb_buffer.buffer );
        return false;
    }

    memcpy( out_ppm_buffer->buffer, header, (size_t)header_len );
    memcpy( (char*)out_ppm_buffer->buffer + header_len, rgb_buffer.buffer, rgb_buffer.size );

    allocator->Free( rgb_buffer.buffer );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_gpr_to_jpg(const gpr_allocator*        allocator,
                                  GPR_RGB_RESOLUTION    rgb_resolution,
                                  int                   jpg_quality,
                                  gpr_buffer*           inp_gpr_buffer,
                                  gpr_buffer*           out_jpg_buffer)
{
    TIMESTAMP("[BEG]", 2)

#if GPR_JPEG_AVAILABLE
    gpr_rgb_buffer rgb_buffer = { NULL, 0, 0, 0 };
    gpr_parameters params;
    int  exif_orientation = 1;
    bool ok;

    // JPG output is always 8-bit interleaved RGB.
    if( gpr_convert_gpr_to_rgb( allocator, rgb_resolution, 8, inp_gpr_buffer, &rgb_buffer ) == false )
        return false;

    // Carry the image orientation into the JPG as an EXIF tag (metadata only, no pixel rotation)
    // so the JPG displays the same way up as the source GPR/DNG.
    gpr_parameters_set_defaults( &params );

    if( gpr_parameters_parse_dng( allocator, inp_gpr_buffer, &params ) )
        exif_orientation = gpr_adobe_orientation_to_exif( (int)params.tuning_info.orientation );

    ok = gpr_encode_rgb_to_jpg( allocator, (const unsigned char*)rgb_buffer.buffer,
                                (int)rgb_buffer.width, (int)rgb_buffer.height,
                                jpg_quality, exif_orientation, out_jpg_buffer );

    gpr_parameters_destroy( &params, allocator->Free );
    allocator->Free( rgb_buffer.buffer );

    TIMESTAMP("[END]", 1)

    return ok;
#else
    (void)allocator; (void)rgb_resolution; (void)jpg_quality; (void)inp_gpr_buffer; (void)out_jpg_buffer;
    return false;
#endif
}

// A thumbnail for the plain-DNG path to embed (see write_dng), decoded from the source at
// preview_resolution. Costs one reduced-resolution decode, so callers opt in by setting it.
//
// The same decode and encode gpr_convert_gpr_to_jpg makes, so the thumbnail matches the JPGs
// taken of the same file, but at EXIF orientation 1 rather than the source's: the DNG's own
// Orientation tag already covers everything in the file, thumbnail included, and a second one
// here would turn a rotated shot twice.
static bool decode_preview_jpg(const gpr_allocator*    allocator,
                               const gpr_parameters*   parameters,
                                     gpr_buffer*       inp_gpr_buffer,
                                     gpr_buffer_auto*  out_jpg_buffer)
{
#if GPR_READING && GPR_JPEG_AVAILABLE
    gpr_rgb_buffer  rgb_buffer = { NULL, 0, 0, 0 };
    gpr_buffer      jpg_buffer = { NULL, 0 };
    bool            ok;

    if( parameters->enable_preview == false ||
        parameters->preview_resolution == GPR_RGB_RESOLUTION_NONE ||
        gpr_convert_gpr_to_rgb( allocator, parameters->preview_resolution, 8,
                                inp_gpr_buffer, &rgb_buffer ) == false )
    {
        return false;
    }

    ok = gpr_encode_rgb_to_jpg( allocator, (const unsigned char*)rgb_buffer.buffer,
                                (int)rgb_buffer.width, (int)rgb_buffer.height,
                                2, 1, &jpg_buffer );

    allocator->Free( rgb_buffer.buffer );

    if( ok )
        out_jpg_buffer->set( jpg_buffer.buffer, jpg_buffer.size, true );

    return ok;
#else
    (void)allocator; (void)parameters; (void)inp_gpr_buffer; (void)out_jpg_buffer;
    return false;
#endif
}

bool gpr_convert_gpr_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_gpr_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 2)

    try
    {
        gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
        gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);

        // Read-only view over the caller's buffer - no copy of the input file
        dng_stream inp_gpr_stream( inp_gpr_buffer->buffer, (uint32)inp_gpr_buffer->size );

        // The thumbnail to embed (see write_dng): the one the input already carries, when it
        // has one - cheaper than decoding, and closer to what the source looks like -
        // otherwise one decoded below. GoPro cameras write no preview, so in practice the read
        // finds one only for a DNG that has been through this conversion before, or a raw from
        // something else (an iPhone DNG carries its rendered frame here).
        gpr_buffer_auto preview_jpg(allocator->Alloc, allocator->Free);
        gpr_parameters  preview_params;

        if( read_dng( allocator, &inp_gpr_stream, &raw_buffer, &vc5_buffer, NULL, NULL, NULL, &preview_jpg ) == false )
        {
            return false;
        }

        if( preview_jpg.is_valid() == false )
            decode_preview_jpg( allocator, parameters, inp_gpr_buffer, &preview_jpg );

        dng_memory_stream out_dng_stream( gDefaultDNGMemoryAllocator );

        write_dng( allocator, &out_dng_stream, &raw_buffer, false, NULL,
                   attach_preview( parameters, &preview_params, &preview_jpg ) );

        write_dngstream_to_buffer( &out_dng_stream, out_dng_buffer, allocator->Alloc, allocator->Free );
    }
    catch( ... ) // the DNG SDK throws dng_exception on malformed input; C callers expect false
    {
        return false;
    }

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_vc5_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_vc5_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_buffer_auto vc5_buffer( allocator->Alloc, allocator->Free );
    
    vc5_buffer.set(inp_vc5_buffer->buffer, inp_vc5_buffer->size);
    
    dng_memory_stream out_dng_stream( gDefaultDNGMemoryAllocator );
    
    write_dng( allocator, &out_dng_stream, NULL, false, &vc5_buffer, parameters );

    write_dngstream_to_buffer( &out_dng_stream, out_dng_buffer, allocator->Alloc, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_gpr_to_raw(const gpr_allocator*            allocator,
                                  gpr_buffer*               inp_gpr_buffer,
                                  gpr_buffer*               out_raw_buffer)
{
    TIMESTAMP("[BEG]", 2)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    // Read-only view over the caller's buffer - no copy of the input file
    dng_stream inp_gpr_stream( inp_gpr_buffer->buffer, (uint32)inp_gpr_buffer->size );
    
    if( read_dng( allocator, &inp_gpr_stream, &raw_buffer, NULL, NULL, NULL ) == false )
    {
        assert(0); return false;
    }
    
    out_raw_buffer->buffer = allocator->Alloc( raw_buffer.get_size() );
    out_raw_buffer->size = raw_buffer.get_size();
    
    memcpy(out_raw_buffer->buffer, raw_buffer.get_buffer(), raw_buffer.get_size() );
    
    TIMESTAMP("[END]", 1)

    return true;
}

#endif // GPR_READING

bool gpr_check_vc5( const gpr_allocator*        allocator,
                          gpr_buffer*           inp_dng_buffer)
{
    TIMESTAMP("[BEG]", 2)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    bool is_vc5_format = false;
    
    {
        // Read-only view over the caller's buffer - no copy of the input file
        dng_stream inp_dng_stream( inp_dng_buffer->buffer, (uint32)inp_dng_buffer->size );

        if( read_dng( allocator, &inp_dng_stream, &raw_buffer, &vc5_buffer, NULL, &is_vc5_format ) == false )
        {
            assert(0); return -1;
        }
    }
    
    TIMESTAMP("[END]", 1)
    
    return is_vc5_format;
}


