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

#if GPR_READING
#include "vc5_decoder.h"
#endif

#if GPR_WRITING
#include "gpr_image_writer.h"
#endif

#if GPR_READING
#include "gpr_read_image.h"
#endif

#if GPR_JPEG_AVAILABLE
#include "jpeg.h"
#endif

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

#if GPR_WRITING
static void set_vc5_encoder_parameters( vc5_encoder_parameters& vc5_encoder_params, const gpr_parameters* convert_params )
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

        case PIXEL_FORMAT_RGGB_16:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_16;
            break;

        case PIXEL_FORMAT_GBRG_16:
            vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_GBRG_16;
            break;
            
        default:
            break;
    }
    
    if( convert_params->quality >= 0 && convert_params->quality < VC5_ENCODER_QUALITY_SETTING_COUNT )
        vc5_encoder_params.quality_setting = (VC5_ENCODER_QUALITY_SETTING)convert_params->quality;
    else if( convert_params->fast_encoding )
        vc5_encoder_params.quality_setting = VC5_ENCODER_QUALITY_SETTING_MEDIUM;
    else
        vc5_encoder_params.quality_setting = VC5_ENCODER_QUALITY_SETTING_FS1;

    vc5_encoder_params.denoise_enabled    = convert_params->tuning_info.denoise_enabled;
    vc5_encoder_params.denoise_strength   = convert_params->tuning_info.denoise_strength;
    vc5_encoder_params.noise_scale        = convert_params->tuning_info.noise_scale;
    vc5_encoder_params.noise_offset       = convert_params->tuning_info.noise_offset;
    vc5_encoder_params.variance_stabilize = convert_params->tuning_info.variance_stabilize;
    vc5_encoder_params.ans_enabled       = convert_params->tuning_info.ans_enabled;

}
#endif

void gpr_parameters_set_defaults(gpr_parameters* x)
{
    memset(x, 0, sizeof(gpr_parameters));
    
    gpr_exif_info_set_defaults(&x->exif_info);
    gpr_profile_info_set_defaults(&x->profile_info);
    gpr_tuning_info_set_defaults(&x->tuning_info);

    x->enable_preview = true;
    
    x->compute_md5sum = false;
    
    x->fast_encoding = false;
    x->quality = -1;
}

void gpr_parameters_construct_copy(const gpr_parameters* y, gpr_parameters* x, gpr_malloc mem_alloc)
{
    gpr_parameters_set_defaults(x);

    *x = *y;

    // Clear FPN heap pointers to prevent double-free (shallow copy shares pointers)
    // The copy does NOT own the FPN allocations — only the original does.
    x->fpn.row_offsets[0] = NULL; x->fpn.row_offsets[1] = NULL;
    x->fpn.row_offsets[2] = NULL; x->fpn.row_offsets[3] = NULL;
    x->fpn.col_offsets[0] = NULL; x->fpn.col_offsets[1] = NULL;
    x->fpn.col_offsets[2] = NULL; x->fpn.col_offsets[3] = NULL;
    x->fpn.precomputed_map = NULL;

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

    // Deep-copy HueSatMap data
    if( y->profile_info.hue_sat_map_dims[0] > 0 )
    {
        uint64_t count64 = (uint64_t)y->profile_info.hue_sat_map_dims[0] *
                           y->profile_info.hue_sat_map_dims[1] *
                           y->profile_info.hue_sat_map_dims[2];
        /* Sanity check: reject absurdly large maps (> ~8M entries = ~96MB) */
        if( count64 > 8000000 ) count64 = 0;
        uint32_t count = (uint32_t)count64;
        size_t data_size = count * 3 * sizeof(float);

        if( y->profile_info.hue_sat_map_data1 != NULL )
        {
            x->profile_info.hue_sat_map_data1 = (float *)mem_alloc( data_size );
            memcpy( x->profile_info.hue_sat_map_data1, y->profile_info.hue_sat_map_data1, data_size );
        }
        if( y->profile_info.hue_sat_map_data2 != NULL )
        {
            x->profile_info.hue_sat_map_data2 = (float *)mem_alloc( data_size );
            memcpy( x->profile_info.hue_sat_map_data2, y->profile_info.hue_sat_map_data2, data_size );
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

    if( x->profile_info.hue_sat_map_data1 )
    {
        mem_free( x->profile_info.hue_sat_map_data1 );
        x->profile_info.hue_sat_map_data1 = NULL;
    }
    if( x->profile_info.hue_sat_map_data2 )
    {
        mem_free( x->profile_info.hue_sat_map_data2 );
        x->profile_info.hue_sat_map_data2 = NULL;
    }

    fpn_model_free(&x->fpn);
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
    
    {
        size_t len = src_exif->fSoftware.Length();
        if (len >= sizeof(dst_exif->software_version))
            len = sizeof(dst_exif->software_version) - 1;
        memcpy( dst_exif->software_version, src_exif->fSoftware.Get(), len );
        dst_exif->software_version[len] = '\0';
    }

    {
        size_t len = src_exif->fUserComment.Length();
        if (len >= sizeof(dst_exif->user_comment))
            len = sizeof(dst_exif->user_comment) - 1;
        memcpy( dst_exif->user_comment, src_exif->fUserComment.Get(), len );
        dst_exif->user_comment[len] = '\0';
    }
    
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


#define MAX_BUF_SIZE 16000

static char _warp_rect_buffer [256];

static bool read_dng(const gpr_allocator*       allocator,
                           dng_stream*          dng_read_stream,
                           gpr_buffer_auto*     raw_image_buffer,
                           gpr_buffer_auto*     vc5_image_buffer,
                           gpr_parameters*      convert_params = NULL,
                           bool*                is_vc5_format = NULL )
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
        
        negative.Reset (host.Make_dng_negative ());
        
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
            if (negative->ProfileCount() > 0)
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

                convert_params->profile_info.compute_color_matrix = false;
                convert_params->profile_info.matrix_weighting = 1.0;

                memset( convert_params->profile_info.wb1, 0, sizeof(convert_params->profile_info.wb1) );
                memset( convert_params->profile_info.wb2, 0, sizeof(convert_params->profile_info.wb2) );

                memset( convert_params->profile_info.cam_to_srgb_1, 0, sizeof(convert_params->profile_info.cam_to_srgb_1) );
                memset( convert_params->profile_info.cam_to_srgb_2, 0, sizeof(convert_params->profile_info.cam_to_srgb_2) );

                // Copy CalibrationIlluminant values
                convert_params->profile_info.illuminant1 = profile_info.CalibrationIlluminant1();
                convert_params->profile_info.illuminant2 = profile_info.CalibrationIlluminant2();

                // Copy ForwardMatrix if present
                const dng_matrix &fm1 = profile_info.ForwardMatrix1();
                const dng_matrix &fm2 = profile_info.ForwardMatrix2();
                if (!fm1.IsEmpty() && !fm2.IsEmpty())
                {
                    convert_params->profile_info.has_forward_matrix = true;
                    for (i = 0; i < 3; i++)
                        for (j = 0; j < 3; j++)
                        {
                            convert_params->profile_info.forward_matrix_1[i][j] = fm1[i][j];
                            convert_params->profile_info.forward_matrix_2[i][j] = fm2[i][j];
                        }
                }
                else
                {
                    convert_params->profile_info.has_forward_matrix = false;
                }
            }

            // Copy BaselineExposure and AnalogBalance
            {
                convert_params->profile_info.baseline_exposure = negative->BaselineExposure();

                convert_params->profile_info.analog_balance[0] = negative->AnalogBalance(0);
                convert_params->profile_info.analog_balance[1] = negative->AnalogBalance(1);
                convert_params->profile_info.analog_balance[2] = negative->AnalogBalance(2);
            }

            // Copy ProfileHueSatMapData if present
            if (negative->ProfileCount() > 0)
            {
                const dng_camera_profile &cam_profile = negative->ProfileByIndex( 0 );
                if (cam_profile.HasHueSatDeltas())
                {
                    const dng_hue_sat_map &hsm1 = cam_profile.HueSatDeltas1();
                    uint32 hDiv, sDiv, vDiv;
                    hsm1.GetDivisions(hDiv, sDiv, vDiv);
                    uint64 count64 = (uint64)hDiv * sDiv * vDiv;

                    convert_params->profile_info.hue_sat_map_dims[0] = hDiv;
                    convert_params->profile_info.hue_sat_map_dims[1] = sDiv;
                    convert_params->profile_info.hue_sat_map_dims[2] = vDiv;
                    convert_params->profile_info.hue_sat_map_encoding = cam_profile.HueSatMapEncoding();

                    /* Sanity check: reject absurdly large maps */
                    if (count64 > 8000000)
                    {
                        count64 = 0;
                        convert_params->profile_info.hue_sat_map_dims[0] = 0;
                        convert_params->profile_info.hue_sat_map_dims[1] = 0;
                        convert_params->profile_info.hue_sat_map_dims[2] = 0;
                    }
                    uint32 count = (uint32)count64;

                    // Copy data1 (3 floats per entry: hueShift, satScale, valScale)
                    size_t data_size = count * 3 * sizeof(float);
                    convert_params->profile_info.hue_sat_map_data1 = (float *)allocator->Alloc(data_size);
                    const dng_hue_sat_map::HSBModify *src1 = hsm1.GetConstDeltas();
                    memcpy(convert_params->profile_info.hue_sat_map_data1, src1, data_size);

                    // Copy data2 if present
                    const dng_hue_sat_map &hsm2 = cam_profile.HueSatDeltas2();
                    if (hsm2.IsValid())
                    {
                        convert_params->profile_info.hue_sat_map_data2 = (float *)allocator->Alloc(data_size);
                        const dng_hue_sat_map::HSBModify *src2 = hsm2.GetConstDeltas();
                        memcpy(convert_params->profile_info.hue_sat_map_data2, src2, data_size);
                    }
                }
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
                
                const dng_linearization_info *linearization_ptr = negative->GetLinearizationInfo();
                if (linearization_ptr != NULL)
                {
                    const dng_linearization_info& linearization_info = *linearization_ptr;

                    gpr_static_black_level& static_black_level    = tuning_info.static_black_level;

                    // Index with modulo repeat dims so that a 1x1 pattern
                    // (uniform black level) replicates to all four channels
                    uint32 rr = linearization_info.fBlackLevelRepeatRows;
                    uint32 rc = linearization_info.fBlackLevelRepeatCols;
                    if (rr < 1) rr = 1;
                    if (rc < 1) rc = 1;

                    static_black_level.r_black   = linearization_info.fBlackLevel[0 % rr][0 % rc][0];
                    static_black_level.g_r_black = linearization_info.fBlackLevel[0 % rr][1 % rc][0];
                    static_black_level.g_b_black = linearization_info.fBlackLevel[1 % rr][0 % rc][0];
                    static_black_level.b_black   = linearization_info.fBlackLevel[1 % rr][1 % rc][0];

                    gpr_saturation_level& dgain_saturation_level = tuning_info.dgain_saturation_level;

                    dgain_saturation_level.level_red        = linearization_info.fWhiteLevel[0];
                    dgain_saturation_level.level_green_even = dgain_saturation_level.level_red;
                    dgain_saturation_level.level_green_odd  = dgain_saturation_level.level_red;
                    dgain_saturation_level.level_blue       = dgain_saturation_level.level_red;
                }
                
                {
                    dng_ifd &rawIFD = *info.fIFD [info.fMainIndex].Get ();
                 
                    gpr_saturation_level& dgain_saturation_level = tuning_info.dgain_saturation_level;
                    
                    bool rggb_raw = (rawIFD.fCFAPattern[0][0] == 0) && (rawIFD.fCFAPattern[0][1] == 1) && (rawIFD.fCFAPattern[1][0] == 1) && (rawIFD.fCFAPattern[1][1] == 2);
                    
                    // Determine bit depth from the maximum saturation level
                    uint32_t max_sat = dgain_saturation_level.level_red;
                    if (dgain_saturation_level.level_green_even > max_sat) max_sat = dgain_saturation_level.level_green_even;
                    if (dgain_saturation_level.level_green_odd  > max_sat) max_sat = dgain_saturation_level.level_green_odd;
                    if (dgain_saturation_level.level_blue       > max_sat) max_sat = dgain_saturation_level.level_blue;

                    if( rggb_raw )
                    {
                        if( max_sat <= 4095 )
                        {
                            tuning_info.pixel_format = PIXEL_FORMAT_RGGB_12;
                        }
                        else if( max_sat <= 16383 )
                        {
                            tuning_info.pixel_format = PIXEL_FORMAT_RGGB_14;
                        }
                        else
                        {
                            tuning_info.pixel_format = PIXEL_FORMAT_RGGB_16;
                        }
                    }
                    else
                    {
                        if( max_sat <= 4095 )
                        {
                            tuning_info.pixel_format = PIXEL_FORMAT_GBRG_12;
                        }
                        else
                        {
                            // No GBRG_14 enum; use GBRG_16 for 14-bit and 16-bit
                            tuning_info.pixel_format = PIXEL_FORMAT_GBRG_16;
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
               }

                // Read noise model metadata from XMP (if encoded with --Denoise)
                {
                    dng_xmp *xmp = negative->GetXMP();
                    if (xmp)
                    {
                        const char *ns = "http://ns.adobe.com/exif/1.0/aux/";
                        uint32 seed = 0;
                        if (xmp->Get_uint32(ns, "GPRNoiseSeed", seed))
                        {
                            tuning_info.noise_seed = seed;
                            real64 sig = 0;
                            if (xmp->Get_real64(ns, "GPRNoiseSigma0", sig)) tuning_info.noise_sigma_est[0] = sig;
                            if (xmp->Get_real64(ns, "GPRNoiseSigma1", sig)) tuning_info.noise_sigma_est[1] = sig;
                            if (xmp->Get_real64(ns, "GPRNoiseSigma2", sig)) tuning_info.noise_sigma_est[2] = sig;
                            if (xmp->Get_real64(ns, "GPRNoiseSigma3", sig)) tuning_info.noise_sigma_est[3] = sig;
                        }
                    }
                }

                // Read FPN polynomial from XMP (if stored during encoding)
                {
                    dng_xmp *xmp2 = negative->GetXMP();
                    if (xmp2)
                    {
                        const char *ns2 = "http://ns.adobe.com/exif/1.0/aux/";
                        uint32 poly_order = 0;
                        if (xmp2->Get_uint32(ns2, "GPRFpnPolyOrder", poly_order) && poly_order > 0)
                        {
                            convert_params->fpn.poly_order = poly_order;
                            uint32 fw = 0, fh = 0;
                            xmp2->Get_uint32(ns2, "GPRFpnWidth", fw);
                            xmp2->Get_uint32(ns2, "GPRFpnHeight", fh);
                            convert_params->fpn.width = fw;
                            convert_params->fpn.height = fh;

                            const char *ch_prefix[] = {"GPRFpnR", "GPRFpnGr", "GPRFpnGb", "GPRFpnB"};
                            for (int ch = 0; ch < 4; ch++)
                            {
                                for (int i = 0; i < FPN_MAX_POLY_TERMS && i < 15; i++)
                                {
                                    char key[64];
                                    snprintf(key, sizeof(key), "%s%d", ch_prefix[ch], i);
                                    real64 val = 0;
                                    xmp2->Get_real64(ns2, key, val);
                                    convert_params->fpn.poly_coeffs[ch][i] = val;
                                }
                            }
                            if (fw > 0 && fh > 0)
                                convert_params->fpn.valid = 1;
                        }
                    }
                }

                // DefaultCropOrigin and DefaultCropSize
                tuning_info.default_crop_origin_h = Round_uint32(negative->DefaultCropOriginH().As_real64());
                tuning_info.default_crop_origin_v = Round_uint32(negative->DefaultCropOriginV().As_real64());
                tuning_info.default_crop_size_h = Round_uint32(negative->DefaultCropSizeH().As_real64());
                tuning_info.default_crop_size_v = Round_uint32(negative->DefaultCropSizeV().As_real64());

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
                    // Get WarpRectilinear Opcode
                    dng_opcode &opcode = opcodelist3.Entry( 0 );
                    
                    dng_stream stream ( _warp_rect_buffer, 256 );
                    opcode.PutData( stream );
                    
                    // Ugly way to get the parameters, but I couldn't figure how else to get access to the data
                    double red_coefficient = * (double *) &_warp_rect_buffer[8];
                    double blue_coefficient = * (double *) &_warp_rect_buffer[8 + 2*6*8];
                    //LogPrint( "WarpRectilinear red = %f, blue = %f ", red_coefficient, blue_coefficient );
                    
                    tuning_info.warp_red_coefficient = red_coefficient;
                    tuning_info.warp_blue_coefficient = blue_coefficient;
                }
                else
                {
                    tuning_info.warp_red_coefficient = 0;
                    tuning_info.warp_blue_coefficient = 0;
                }
            }
        }
        
        if( raw_image_buffer )
        {
            CopyRawImageToBuffer( raw_image, *raw_image_buffer );
        }
    }

    return true;
}


void reduction(double a[][6], int size, int pivot, int col) 
{
   int i, j;
   double factor;
   factor = a[pivot][col];
 
   for (i = 0; i < 2 * size; i++) {
      a[pivot][i] /= factor;
   }
 
   for (i = 0; i < size; i++) {
      if (i != pivot) {
         factor = a[i][col];
         for (j = 0; j < 2 * size; j++) {
            a[i][j] = a[i][j] - a[pivot][j] * factor;
         }
      }
   }
}

void calc_color_matrix( double in_matrix[3][3], double wb[3], double weight, double out_matrix[3][3] )
{
    double temp1[3][3];
    double temp2[3][3];

    int i,j,k;

#if PRINT_MATRIX
    LogPrint("\nOriginal Matrix");
    for (i = 0; i < 3; i++)
      LogPrint("%8.5f  %8.5f  %8.5f", in_matrix[i][0], in_matrix[i][1], in_matrix[i][2] );
#endif
    
    // Interpolate with identity matrix by weight w
    double w = weight;
    double z = 1.0 - weight;

    for (i = 0; i < 3; i++ )
    {
        for (j = 0; j < 3; j++ )
            temp1[i][j] = in_matrix[i][j] * w;

        temp1[i][i] += z;
    }

#if PRINT_MATRIX
    LogPrint("\nInterpolated Matrix");
    for (i = 0; i < 3; i++)
      LogPrint("%8.5f  %8.5f  %8.5f", temp1[i][0], temp1[i][1], temp1[i][2] );
#endif
    
    // Multiply matrix by sRGB_to_XYZd50 (from http://www.brucelindbloom.com)
    double sRGB_to_XYZd50[3][3] = {{0.4361, 0.3851, 0.1431}, {0.2225, 0.7169, 0.0606}, {0.0139, 0.0971, 0.7142}};

    double sum;
    for (i = 0; i < 3; i++) 
        for (j = 0; j < 3; j++) 
        {
            sum = 0;
            for (k = 0; k < 3; k++) 
                sum = sum + sRGB_to_XYZd50[i][k] * temp1[k][j];

            temp2[i][j] = sum;
        }

#if PRINT_MATRIX
    LogPrint("\ntimes  sRGB_to_XYZd50");
    for (i = 0; i < 3; i++)
      LogPrint("%8.5f  %8.5f  %8.5f", temp2[i][0], temp2[i][1], temp2[i][2] );
#endif
    
    // Set up diagonal matrix with white balance gains
    double wb_diag[3][3];
    for (i = 0; i < 3; i++) 
    {
        for (j = 0; j < 3; j++) 
            wb_diag[i][j] = 0.0;
        
        wb_diag[i][i] = wb[i];
    }
    
    // Multiply by white balance gains
    for (i = 0; i < 3; i++) 
        for (j = 0; j < 3; j++) 
        {
            sum = 0;
            for (k = 0; k < 3; k++) 
                sum = sum + temp2[i][k] * wb_diag[k][j];

            temp1[i][j] = sum;
        }

#if PRINT_MATRIX
    LogPrint("\ntimes  wb");
    for (i = 0; i < 3; i++)
      LogPrint("%8.5f  %8.5f  %8.5f", temp1[i][0], temp1[i][1], temp1[i][2] );
#endif
    
    // Invert the resulting matrix
    double matrix[3][6];

    for (i = 0; i < 3; i++)
      for (j = 0; j < 6; j++)
         if (j == i + 3)
            matrix[i][j] = 1;
         else 
            matrix[i][j] = 0;

    for (i = 0; i < 3; i++)
      for (j = 0; j < 3; j++)
         matrix[i][j] = temp1[i][j];

    for (i = 0; i < 3; i++)
      reduction(matrix, 3, i, i);

    for (i = 0; i < 3; i++)
      for (j = 0; j < 3; j++)
         out_matrix[i][j] = matrix[i][j+3];

#if PRINT_MATRIX
    LogPrint("\nInverse Matrix");
    for (i = 0; i < 3; i++) {
       LogPrint("%8.5f  %8.5f  %8.5f", out_matrix[i][0], out_matrix[i][1], out_matrix[i][2] );
    }
#endif

}

typedef struct
{
    unsigned char*     orig_dst;                     /* Address to the memory location that this buffer points to */

    unsigned char*     next_dst;
    
} jpg_write_context;

void write_jpg_thumbnail(void* context, void* data, int size)
{
    jpg_write_context* _context = (jpg_write_context*)context;
    
    memcpy( _context->next_dst, data, size );
    
    _context->next_dst = (unsigned char*)_context->next_dst + size;
}
 
static void write_dng(const gpr_allocator*          allocator,
                            dng_stream*             dng_write_stream,
                      const gpr_buffer_auto*        raw_image_buffer,
                            bool                    compress_raw_to_vc5,
                            gpr_buffer_auto*        vc5_image_buffer,
                      const gpr_parameters*   	    convert_params )
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
    
    AutoPtr<dng_image> image(new dng_simple_image(rect, 1, ttShort, memalloc));
    
    gpr_buffer_auto raw_allocated_buffer( allocator->Alloc, allocator->Free );
    
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

            case PIXEL_FORMAT_RGGB_16:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_RGGB_16;
                break;

            case PIXEL_FORMAT_GBRG_16:
                vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_GBRG_16;
                break;
                        
            default:
                assert(0);
                return;
        };

        vc5_decoder_params.variance_stabilize = convert_params->tuning_info.variance_stabilize;
        vc5_decoder_params.noise_scale        = convert_params->tuning_info.noise_scale;
        vc5_decoder_params.noise_offset       = convert_params->tuning_info.noise_offset;
        vc5_decoder_params.noise_seed         = convert_params->tuning_info.noise_seed;
        vc5_decoder_params.add_noise_back     = (convert_params->tuning_info.noise_seed != 0)
                                                && !convert_params->tuning_info.denoise_output;
        memcpy(vc5_decoder_params.noise_sigma, convert_params->tuning_info.noise_sigma_est,
               sizeof(vc5_decoder_params.noise_sigma));

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
          vc5_dng == false )
    {
        unpack_pixel_format( raw_image_buffer, convert_params, &raw_allocated_buffer );
        
        raw_image_buffer = &raw_allocated_buffer;
        
        input_pitch = convert_params->input_width * 2;
    }
    
    if( vc5_dng == false )
    {
        CopyBufferToRawImage( *raw_image_buffer, input_pitch / sizeof(short), *(image.Get()) );
    }

    AutoPtr<dng_negative> negative(host.Make_dng_negative());
    
    negative->SetOriginalBestQualityFinalSize( dng_point(outputHeight, outputWidth) );

    negative->SetOriginalDefaultFinalSize( dng_point(outputHeight, outputWidth) );
    
    { // Set Tuning Info
        const gpr_tuning_info*  tuning_info       = &convert_params->tuning_info;

        const gpr_static_black_level static_black_level    = tuning_info->static_black_level;
        
        switch( convert_params->tuning_info.pixel_format )
        {
            case PIXEL_FORMAT_RGGB_12:
            case PIXEL_FORMAT_RGGB_12P:
            case PIXEL_FORMAT_RGGB_14:
            case PIXEL_FORMAT_RGGB_16:
                negative->SetQuadBlacks(static_black_level.r_black,
                                        static_black_level.g_r_black,
                                        static_black_level.g_b_black,
                                        static_black_level.b_black,
                                        -1 );
                break;
            case PIXEL_FORMAT_GBRG_12:
            case PIXEL_FORMAT_GBRG_12P:
            case PIXEL_FORMAT_GBRG_16:
                negative->SetQuadBlacks(static_black_level.g_b_black,
                                        static_black_level.b_black,
                                        static_black_level.r_black,
                                        static_black_level.g_r_black,
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
            
            dng_stream gain_map_stream0 (tuning_info->gain_map.buffers[0], gain_map_size);
            AutoPtr<dng_opcode> gain_map_opcode0 ( new dng_opcode_GainMap ( host, gain_map_stream0 ));
            opcodelist2.Append( gain_map_opcode0 );
    
            dng_stream gain_map_stream1 (tuning_info->gain_map.buffers[1], gain_map_size);
            AutoPtr<dng_opcode> gain_map_opcode1 ( new dng_opcode_GainMap ( host, gain_map_stream1 ));
            opcodelist2.Append( gain_map_opcode1 );
    
            dng_stream gain_map_stream2 (tuning_info->gain_map.buffers[2], gain_map_size);
            AutoPtr<dng_opcode> gain_map_opcode2 ( new dng_opcode_GainMap ( host, gain_map_stream2 ));
            opcodelist2.Append( gain_map_opcode2 );
    
            dng_stream gain_map_stream3 (tuning_info->gain_map.buffers[3], gain_map_size);
            AutoPtr<dng_opcode> gain_map_opcode3 ( new dng_opcode_GainMap ( host, gain_map_stream3 ));
            opcodelist2.Append( gain_map_opcode3 );
        }

       // WarpRectilinear - aka chromatic aberration correction (applied after demosaicking (OpcodeList3))
        if ( tuning_info->warp_red_coefficient > 0 && tuning_info->warp_blue_coefficient > 0 )
        {
            dng_opcode_list &opcodelist3 = negative->OpcodeList3 ();
            
            dng_warp_params_rectilinear chromatic_aberration;
            
            chromatic_aberration.fPlanes = 3;
            chromatic_aberration.fCenter = dng_point_real64( 0.5, 0.5 );
            chromatic_aberration.fRadParams[0][0] = tuning_info->warp_red_coefficient;
            chromatic_aberration.fRadParams[1][0] = 1.0;
            chromatic_aberration.fRadParams[2][0] = tuning_info->warp_blue_coefficient;
            
            AutoPtr<dng_opcode> warp_opcode ( new dng_opcode_WarpRectilinear ( chromatic_aberration, 0x03 ));
    
            opcodelist3.Append( warp_opcode );
        }
    }
    
    //GP!! NEED outputWidth, activeWidth, outputHeight, activeHeight here
    negative->SetDefaultScale(dng_urational(outputWidth, activeWidth), dng_urational(outputHeight, activeHeight));
    
    uint32 crop_origin_h = convert_params->tuning_info.default_crop_origin_h;
    uint32 crop_origin_v = convert_params->tuning_info.default_crop_origin_v;
    dng_point crop_origin( crop_origin_v, crop_origin_h );

    /* Use stored crop size when available; fall back to symmetric formula */
    uint32 crop_size_h = convert_params->tuning_info.default_crop_size_h;
    uint32 crop_size_v = convert_params->tuning_info.default_crop_size_v;
    if (crop_size_h == 0) crop_size_h = activeWidth  - 2 * crop_origin_h;
    if (crop_size_v == 0) crop_size_v = activeHeight - 2 * crop_origin_v;
    dng_point crop_size( crop_size_v, crop_size_h );

    negative->SetDefaultCropOrigin( crop_origin.h, crop_origin.v );
    negative->SetDefaultCropSize( crop_size.h, crop_size.v );

    negative->SetOriginalDefaultCropSize( dng_urational(crop_size.h, 1), dng_urational(crop_size.v, 1) );

    {
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
    if( convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_12 || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_12P || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_14 || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_RGGB_16 )
    {
        negative->SetBayerMosaic(1);
    }
    else if( convert_params->tuning_info.pixel_format == PIXEL_FORMAT_GBRG_12 || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_GBRG_12P || convert_params->tuning_info.pixel_format == PIXEL_FORMAT_GBRG_16 )
    {
        negative->SetBayerMosaic(3);
    }
    else
    {
        assert(0);
        return;
    }
    
    negative->SetBaselineExposure(profile_info->baseline_exposure);
    negative->SetBaselineNoise(1.0);
    negative->SetBaselineSharpness(1.0);

    negative->SetAntiAliasStrength(dng_urational(100, 100));
    negative->SetLinearResponseLimit(1.0);
    negative->SetShadowScale( dng_urational(1, 1) );
    negative->SetAnalogBalance(dng_vector_3(profile_info->analog_balance[0],
                                            profile_info->analog_balance[1],
                                            profile_info->analog_balance[2]));
    
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
    
        calc_color_matrix( profile_info->cam_to_srgb_1, profile_info->wb1, matrix_weighting, out_matrix );
    
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
                mColor1[i][j] = out_matrix[i][j];
    
        calc_color_matrix( profile_info->cam_to_srgb_2, profile_info->wb2, matrix_weighting, out_matrix );
    
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

#if PRINT_MATRIX
    LogPrint("CM1:");
    for (i = 0; i < 3; i++)
            LogPrint("  %8.5f  %8.5f  %8.5f", mColor1[i][0], mColor1[i][1], mColor1[i][2] );
    LogPrint("CM2:");
    for (i = 0; i < 3; i++)
            LogPrint("  %8.5f  %8.5f  %8.5f", mColor2[i][0], mColor2[i][1], mColor2[i][2] );
#endif
    
    prof->SetColorMatrix1((dng_matrix) mColor1);
    prof->SetColorMatrix2((dng_matrix) mColor2);
    
    prof->SetCalibrationIlluminant1(profile_info->illuminant1);
    prof->SetCalibrationIlluminant2(profile_info->illuminant2);

    if (profile_info->has_forward_matrix)
    {
        dng_matrix_3by3 fm1;
        dng_matrix_3by3 fm2;
        for (i = 0; i < 3; i++)
            for (j = 0; j < 3; j++)
            {
                fm1[i][j] = profile_info->forward_matrix_1[i][j];
                fm2[i][j] = profile_info->forward_matrix_2[i][j];
            }
        prof->SetForwardMatrix1(fm1);
        prof->SetForwardMatrix2(fm2);
    }

    if (profile_info->hue_sat_map_dims[0] > 0 && profile_info->hue_sat_map_data1 != NULL)
    {
        uint32 hDiv = profile_info->hue_sat_map_dims[0];
        uint32 sDiv = profile_info->hue_sat_map_dims[1];
        uint32 vDiv = profile_info->hue_sat_map_dims[2];
        uint32 count = hDiv * sDiv * vDiv;

        dng_hue_sat_map hsm1;
        hsm1.SetDivisions(hDiv, sDiv, vDiv);
        dng_hue_sat_map::HSBModify *dst1 = hsm1.GetDeltas();
        memcpy(dst1, profile_info->hue_sat_map_data1, count * 3 * sizeof(float));
        prof->SetHueSatDeltas1(hsm1);

        if (profile_info->hue_sat_map_data2 != NULL)
        {
            dng_hue_sat_map hsm2;
            hsm2.SetDivisions(hDiv, sDiv, vDiv);
            dng_hue_sat_map::HSBModify *dst2 = hsm2.GetDeltas();
            memcpy(dst2, profile_info->hue_sat_map_data2, count * 3 * sizeof(float));
            prof->SetHueSatDeltas2(hsm2);
        }

        prof->SetHueSatMapEncoding(profile_info->hue_sat_map_encoding);
    }

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
    if( vc5_dng )
    {
        gpr_image_writer* gpr_writer = new gpr_image_writer(raw_image_buffer, convert_params->input_width, convert_params->input_height, convert_params->input_pitch, vc5_image_buffer );
        set_vc5_encoder_parameters( gpr_writer->GetVc5EncoderParams(), convert_params );
      
        gpr_writer->EncodeVc5Image();

        // Write noise model metadata to XMP (after encoding populates sigma/seed)
        {
            vc5_encoder_parameters& enc_params = gpr_writer->GetVc5EncoderParams();
            uint32_t noise_seed = enc_params.noise_seed;
            if (noise_seed == 0) noise_seed = convert_params->tuning_info.noise_seed;
            if (noise_seed != 0)
            {
                dng_xmp *xmp = negative->GetXMP();
                if (xmp)
                {
                    const char *ns = "http://ns.adobe.com/exif/1.0/aux/";
                    xmp->Set_uint32(ns, "GPRNoiseSeed", noise_seed);
                    xmp->Set_real64(ns, "GPRNoiseSigma0", enc_params.noise_sigma_out[0], 6);
                    xmp->Set_real64(ns, "GPRNoiseSigma1", enc_params.noise_sigma_out[1], 6);
                    xmp->Set_real64(ns, "GPRNoiseSigma2", enc_params.noise_sigma_out[2], 6);
                    xmp->Set_real64(ns, "GPRNoiseSigma3", enc_params.noise_sigma_out[3], 6);
                }
            }

            // Write FPN polynomial coefficients if calibration was used
            if (convert_params->fpn.valid)
            {
                dng_xmp *xmp = negative->GetXMP();
                if (xmp)
                {
                    const char *ns = "http://ns.adobe.com/exif/1.0/aux/";
                    xmp->Set_uint32(ns, "GPRFpnPolyOrder", (uint32)convert_params->fpn.poly_order);
                    xmp->Set_uint32(ns, "GPRFpnWidth", (uint32)convert_params->fpn.width);
                    xmp->Set_uint32(ns, "GPRFpnHeight", (uint32)convert_params->fpn.height);
                    const char *ch_prefix[] = {"GPRFpnR", "GPRFpnGr", "GPRFpnGb", "GPRFpnB"};
                    for (int ch = 0; ch < 4; ch++)
                    {
                        for (int i = 0; i < FPN_MAX_POLY_TERMS && i < 15; i++)
                        {
                            char key[64];
                            snprintf(key, sizeof(key), "%s%d", ch_prefix[ch], i);
                            xmp->Set_real64(ns, key, convert_params->fpn.poly_coeffs[ch][i], 8);
                        }
                    }
                }
            }
        }

        if( convert_params->enable_preview )
        {
            const gpr_preview_image& preview_image = convert_params->preview_image;
            
            if( preview_image.jpg_preview.size > 0 && preview_image.jpg_preview.buffer != NULL )
            {
                preview_list = new dng_preview_list;
                
                AutoPtr<dng_jpeg_preview> jpeg_preview;
                jpeg_preview.Reset(new dng_jpeg_preview);
                jpeg_preview->fPhotometricInterpretation = piYCbCr;
                
                jpeg_preview->fInfo.fIsPrimary = true;
                
                jpeg_preview->fPreviewSize.v             = preview_image.preview_height;
                jpeg_preview->fPreviewSize.h             = preview_image.preview_width;
                jpeg_preview->fCompressedData.Reset(host.Allocate( preview_image.jpg_preview.size ));
                memcpy( jpeg_preview->fCompressedData->Buffer_char(), preview_image.jpg_preview.buffer, preview_image.jpg_preview.size );
                
                AutoPtr<dng_preview> pp( dynamic_cast<dng_preview*>(jpeg_preview.Release()) );
                
                preview_list->Append(pp);
            }
#if GPR_JPEG_AVAILABLE
            else
            {
                preview_list = new dng_preview_list;
                
                AutoPtr<dng_jpeg_preview> jpeg_preview;
                jpeg_preview.Reset(new dng_jpeg_preview);
                jpeg_preview->fPhotometricInterpretation = piYCbCr;
                
                jpeg_preview->fInfo.fIsPrimary = true;
                
                const gpr_rgb_buffer& rgb_buffer = gpr_writer->get_rgb_thumbnail();
                
                gpr_buffer_auto buffer( allocator->Alloc, allocator->Free );
                
                buffer.allocate(1024*1024);

                jpg_write_context context;
                context.orig_dst = buffer.to_uchar();
                context.next_dst = context.orig_dst;
                
                tje_encode_with_func(write_jpg_thumbnail, (void*)&context, 2, rgb_buffer.width, rgb_buffer.height, 3, (const unsigned char*)rgb_buffer.buffer );
                
                size_t size = context.next_dst - context.orig_dst;
                jpeg_preview->fPreviewSize.v             = rgb_buffer.height;
                jpeg_preview->fPreviewSize.h             = rgb_buffer.width;
                jpeg_preview->fCompressedData.Reset(host.Allocate( size ));
                memcpy( jpeg_preview->fCompressedData->Buffer_char(), buffer.get_buffer(), size );

                AutoPtr<dng_preview> pp( dynamic_cast<dng_preview*>(jpeg_preview.Release()) );
                
                preview_list->Append(pp);
            }
#endif
        }
        
        writer = gpr_writer;
    }
    else
#endif
    {
        writer = new dng_image_writer;
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

bool gpr_parse_metadata(const gpr_allocator*        allocator,
                              gpr_buffer*           inp_dng_buffer,
                              gpr_parameters*       parameters)
{
    dng_memory_stream inp_dng_stream( gDefaultDNGMemoryAllocator );
    inp_dng_stream.Put( inp_dng_buffer->buffer, inp_dng_buffer->size );
    inp_dng_stream.SetReadPosition(0);
    
    if( read_dng( allocator, &inp_dng_stream, NULL, NULL, parameters ) == false )
    {
        assert(0); return false;
    }
    
    return true;
}

bool gpr_convert_raw_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_raw_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 1)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    raw_buffer.set( (char*)inp_raw_buffer->buffer, inp_raw_buffer->size );
    
    dng_memory_stream out_dng_stream( gDefaultDNGMemoryAllocator );
    
    write_dng( allocator, &out_dng_stream, &raw_buffer, false, NULL, parameters );
    
    write_dngstream_to_buffer( &out_dng_stream, out_dng_buffer, allocator->Alloc, allocator->Free );
    
    TIMESTAMP("[END]", 1)
    
    return true;
}

bool gpr_convert_dng_to_raw(const gpr_allocator*    allocator,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_raw_buffer)
{
    TIMESTAMP("[BEG]", 1)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    dng_memory_stream inp_dng_stream( gDefaultDNGMemoryAllocator );
    inp_dng_stream.Put( inp_dng_buffer->buffer, inp_dng_buffer->size );
    inp_dng_stream.SetReadPosition(0);
    
    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL ) == false )
    {
        assert(0); return false;
    }
    
    out_raw_buffer->buffer = allocator->Alloc( raw_buffer.get_size() );
    out_raw_buffer->size = raw_buffer.get_size();
    
    memcpy(out_raw_buffer->buffer, raw_buffer.get_buffer(), raw_buffer.get_size() );
    
    TIMESTAMP("[END]", 1)
    
    return true;
}

//!< dng to raw conversion
bool gpr_convert_dng_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 1)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    dng_memory_stream inp_dng_stream( gDefaultDNGMemoryAllocator );
    inp_dng_stream.Put( inp_dng_buffer->buffer, inp_dng_buffer->size );
    inp_dng_stream.SetReadPosition(0);
    
    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL ) == false )
    {
        assert(0); return false;
    }
    
    dng_memory_stream out_dng_stream( gDefaultDNGMemoryAllocator );
    
    write_dng( allocator, &out_dng_stream, &raw_buffer, false, NULL, parameters );
    
    write_dngstream_to_buffer( &out_dng_stream, out_dng_buffer, allocator->Alloc, allocator->Free );
    
    TIMESTAMP("[END]", 1)
    
    return true;
}

bool gpr_convert_vc5_to_gpr(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_vc5_buffer,
                                  gpr_buffer*       out_gpr_buffer)
{
    TIMESTAMP("[BEG]", 1)

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
    TIMESTAMP("[BEG]", 1)
    
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    
    dng_memory_stream inp_gpr_stream( gDefaultDNGMemoryAllocator );
    inp_gpr_stream.Put( inp_gpr_buffer->buffer, inp_gpr_buffer->size );
    inp_gpr_stream.SetReadPosition(0);
    
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
    TIMESTAMP("[BEG]", 1)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    raw_buffer.set(inp_raw_buffer->buffer, inp_raw_buffer->size);

    // Phase C: Subtract fixed-pattern noise before encoding (if calibration loaded)
    if (parameters->fpn.valid)
    {
        fpn_subtract(&parameters->fpn, (uint16_t*)raw_buffer.get_buffer(),
                     parameters->input_width, parameters->input_height);
    }

    // Pixel-domain noise removal: quantize each pixel to the noise floor.
    // The compressor then sees a clean signal with minimal entropy.
    // Noise is restored on decode via noise_restore() with the same seed.
    // REQUIRES calibrated noise model (DNG NoiseProfile) — auto-estimation
    // from pixel differences conflates texture with noise and destroys signal.
    if (parameters->tuning_info.noise_replace &&
        parameters->tuning_info.noise_scale > 0)
    {
        // DNG NoiseProfile is in normalized [0,1] units.
        // Convert to raw pixel units: variance_raw = scale * raw * max + offset * max^2
        double max_val = (double)parameters->tuning_info.dgain_saturation_level.level_red;
        if (max_val <= 0) max_val = 16383.0;
        double nr_scale = parameters->tuning_info.noise_scale * max_val;
        double nr_offset = parameters->tuning_info.noise_offset * max_val * max_val;

        if (nr_scale > 0)
        {
            // ENCODER: remove noise only (no PRNG addition).
            // The clean quantized signal compresses much better.
            noise_remove((uint16_t*)raw_buffer.get_buffer(),
                         parameters->input_width, parameters->input_height,
                         nr_scale, nr_offset);

            // Generate a deterministic seed from the raw data for noise restoration
            uint32_t seed = 0x55AA1234;
            const uint16_t *raw16 = (const uint16_t*)raw_buffer.get_buffer();
            for (int i = 0; i < 64 && i < parameters->input_width; i++)
                seed ^= (uint32_t)raw16[i] * 2654435761u;

            // Store seed so the decoder can restore noise.
            // Keep noise_scale/offset in DNG-normalized form (already stored from metadata).
            const_cast<gpr_parameters*>(parameters)->tuning_info.noise_seed = seed;
        }
    }

    dng_memory_stream out_gpr_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_gpr_stream, &raw_buffer, true, NULL, parameters );

    write_dngstream_to_buffer( &out_gpr_stream, out_gpr_buffer, allocator->Alloc, allocator->Free );

    TIMESTAMP("[END]", 1)
    
    return true;
}

bool gpr_convert_dng_to_gpr(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_gpr_buffer)
{
    TIMESTAMP("[BEG]", 1)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);

    dng_memory_stream inp_dng_stream( gDefaultDNGMemoryAllocator );
    inp_dng_stream.Put( inp_dng_buffer->buffer, inp_dng_buffer->size );
    inp_dng_stream.SetReadPosition(0);

    // Extract metadata from input DNG into a mutable copy of parameters
    gpr_parameters params_with_meta;
    gpr_parameters_construct_copy( parameters, &params_with_meta, allocator->Alloc );

    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL, &params_with_meta ) == false )
    {
        gpr_parameters_destroy( &params_with_meta, allocator->Free );
        assert(0); return false;
    }

    dng_memory_stream out_gpr_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_gpr_stream, &raw_buffer, true, NULL, &params_with_meta );

    write_dngstream_to_buffer( &out_gpr_stream, out_gpr_buffer, allocator->Alloc, allocator->Free );

    gpr_parameters_destroy( &params_with_meta, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_dng_to_vc5(const gpr_allocator*    allocator,
                                  gpr_buffer*       inp_dng_buffer,
                                  gpr_buffer*       out_vc5_buffer)
{
    TIMESTAMP("[BEG]", 1)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    
    dng_memory_stream inp_dng_stream( gDefaultDNGMemoryAllocator );
    inp_dng_stream.Put( inp_dng_buffer->buffer, inp_dng_buffer->size );
    inp_dng_stream.SetReadPosition(0);
    
    if( read_dng( allocator, &inp_dng_stream, &raw_buffer, NULL ) == false )
    {
        assert(0); return false;
    }
    
    out_vc5_buffer->buffer = allocator->Alloc( vc5_buffer.get_size() );
    out_vc5_buffer->size = vc5_buffer.get_size();
    
    memcpy(out_vc5_buffer->buffer, vc5_buffer.get_buffer(), vc5_buffer.get_size() );
    
    TIMESTAMP("[END]", 1)

    return true;
}

#endif // GPR_WRITING

#if GPR_READING

bool gpr_convert_gpr_to_rgb(const gpr_allocator*        allocator,
                                  GPR_RGB_RESOLUTION    rgb_resolution,
                                  int                   rgb_bits,
                                  gpr_buffer*           inp_gpr_buffer,
                                  gpr_rgb_buffer*       out_rgb_buffer)
{
    TIMESTAMP("[BEG]", 1)

    gpr_parameters params;
    
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    
    dng_memory_stream inp_gpr_stream( gDefaultDNGMemoryAllocator );
    inp_gpr_stream.Put( inp_gpr_buffer->buffer, inp_gpr_buffer->size );
    inp_gpr_stream.SetReadPosition(0);
    
    if( read_dng( allocator, &inp_gpr_stream, NULL, &vc5_buffer, &params ) == false )
    {
        assert(0); return false;
    }
    
    if( vc5_buffer.is_valid() == false )
    {
        return false;
    }
    
    vc5_decoder_parameters vc5_decoder_params;
    
    vc5_decoder_parameters_set_default(&vc5_decoder_params);
    
    vc5_decoder_params.mem_alloc        = allocator->Alloc;
    vc5_decoder_params.mem_free         = allocator->Free;
    vc5_decoder_params.pixel_format     = VC5_DECODER_PIXEL_FORMAT_DEFAULT;
    
    vc5_decoder_params.rgb_bits = rgb_bits;
    
    gpr_rgb_gain&   rgb_gain = vc5_decoder_params.rgb_gain;
    
    find_rational( params.tuning_info.wb_gains.r_gain, 0.125, &rgb_gain.r_gain_num, &rgb_gain.r_gain_pow2_den );
    find_rational( params.tuning_info.wb_gains.g_gain, 0.125, &rgb_gain.g_gain_num, &rgb_gain.g_gain_pow2_den );
    find_rational( params.tuning_info.wb_gains.b_gain, 0.125, &rgb_gain.b_gain_num, &rgb_gain.b_gain_pow2_den );
    
    vc5_decoder_params.rgb_resolution = rgb_resolution;
    
    if( vc5_decoder_process( &vc5_decoder_params, &vc5_buffer.get_gpr_buffer(), NULL, out_rgb_buffer ) != CODEC_ERROR_OKAY )
    {
        assert(0);
    }

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_gpr_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_gpr_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 1)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);

    dng_memory_stream inp_gpr_stream( gDefaultDNGMemoryAllocator );
    inp_gpr_stream.Put( inp_gpr_buffer->buffer, inp_gpr_buffer->size );
    inp_gpr_stream.SetReadPosition(0);

    // Extract metadata from GPR (which is a DNG) into a mutable copy of parameters
    gpr_parameters params_with_meta;
    gpr_parameters_construct_copy( parameters, &params_with_meta, allocator->Alloc );

    if( read_dng( allocator, &inp_gpr_stream, &raw_buffer, &vc5_buffer, &params_with_meta ) == false )
    {
        gpr_parameters_destroy( &params_with_meta, allocator->Free );
        assert(0); return false;
    }

    dng_memory_stream out_dng_stream( gDefaultDNGMemoryAllocator );

    write_dng( allocator, &out_dng_stream, &raw_buffer, false, NULL, &params_with_meta );

    write_dngstream_to_buffer( &out_dng_stream, out_dng_buffer, allocator->Alloc, allocator->Free );

    gpr_parameters_destroy( &params_with_meta, allocator->Free );

    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_vc5_to_dng(const gpr_allocator*    allocator,
                            const gpr_parameters*   parameters,
                                  gpr_buffer*       inp_vc5_buffer,
                                  gpr_buffer*       out_dng_buffer)
{
    TIMESTAMP("[BEG]", 1)

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
    TIMESTAMP("[BEG]", 1)

    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    
    dng_memory_stream inp_gpr_stream( gDefaultDNGMemoryAllocator );
    inp_gpr_stream.Put( inp_gpr_buffer->buffer, inp_gpr_buffer->size );
    inp_gpr_stream.SetReadPosition(0);
    
    if( read_dng( allocator, &inp_gpr_stream, &raw_buffer, NULL ) == false )
    {
        assert(0); return false;
    }
    
    out_raw_buffer->buffer = allocator->Alloc( raw_buffer.get_size() );
    out_raw_buffer->size = raw_buffer.get_size();
    
    memcpy(out_raw_buffer->buffer, raw_buffer.get_buffer(), raw_buffer.get_size() );
    
    TIMESTAMP("[END]", 1)

    return true;
}

bool gpr_convert_gpr_to_raw_ex(const gpr_allocator*    allocator,
                               const gpr_parameters*   parameters,
                                     gpr_buffer*       inp_gpr_buffer,
                                     gpr_buffer*       out_raw_buffer)
{
    bool result = gpr_convert_gpr_to_raw(allocator, inp_gpr_buffer, out_raw_buffer);

    if (result && parameters && !parameters->tuning_info.denoise_output)
    {
        // Restore noise: add back statistically equivalent noise from PRNG.
        // Triggers automatically when the GPR contains a noise seed (from encoding
        // with -R), unless denoise_output is set (user wants the clean signal).
        if (parameters->tuning_info.noise_scale > 0 &&
            parameters->tuning_info.noise_seed != 0)
        {
            // Convert DNG-normalized noise model to raw pixel units
            double max_val = (double)parameters->tuning_info.dgain_saturation_level.level_red;
            if (max_val <= 0) max_val = 16383.0;
            double nr_scale = parameters->tuning_info.noise_scale * max_val;
            double nr_offset = parameters->tuning_info.noise_offset * max_val * max_val;

            noise_restore((uint16_t*)out_raw_buffer->buffer,
                          parameters->input_width, parameters->input_height,
                          nr_scale, nr_offset,
                          parameters->tuning_info.noise_seed);
        }

        // Restore FPN (fixed-pattern noise)
        if (parameters->fpn.valid)
        {
            fpn_add_back(&parameters->fpn, (uint16_t*)out_raw_buffer->buffer,
                         parameters->input_width, parameters->input_height);
        }
    }

    return result;
}

#endif // GPR_READING

bool gpr_check_vc5( const gpr_allocator*        allocator,
                          gpr_buffer*           inp_dng_buffer)
{
    TIMESTAMP("[BEG]", 1)
    
    gpr_buffer_auto raw_buffer(allocator->Alloc, allocator->Free);
    gpr_buffer_auto vc5_buffer(allocator->Alloc, allocator->Free);
    bool is_vc5_format = false;
    
    {
        dng_memory_stream inp_dng_stream( gDefaultDNGMemoryAllocator );
        inp_dng_stream.Put( inp_dng_buffer->buffer, inp_dng_buffer->size );
        inp_dng_stream.SetReadPosition(0);
        
        if( read_dng( allocator, &inp_dng_stream, &raw_buffer, &vc5_buffer, NULL, &is_vc5_format ) == false )
        {
            assert(0); return -1;
        }
    }
    
    TIMESTAMP("[END]", 1)
    
    return is_vc5_format;
}
