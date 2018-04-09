
#include "gpr_platform.h"
#include "gpr_exif_info.h"
#include "stdc_includes.h"

static gpr_signed_rational signed_rational_construct(int32_t numerator, int32_t denominator)
{
    gpr_signed_rational a;
    a.numerator     = numerator;
    a.denominator   = denominator;
    return a;
}

static gpr_unsigned_rational unsigned_rational_construct(int32_t numerator, int32_t denominator)
{
    gpr_unsigned_rational a;
    a.numerator     = numerator;
    a.denominator   = denominator;
    return a;
}

gpr_date_and_time construct_dng_date_and_time (uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t minute, uint32_t second)
{
    gpr_date_and_time x;
    x.year     = year;
    x.month    = month;
    x.day      = day;
    x.hour     = hour;
    x.minute   = minute;
    x.second   = second;
    
    return x;
}

void gpr_exif_info_set_defaults( gpr_exif_info* x )
{
    x->exposure_time = unsigned_rational_construct(1, 60);
    
    x->exposure_bias = signed_rational_construct( 0, 1 );
    
    double d_aperture = 2.8;
    
    x->f_stop_number = unsigned_rational_construct( (unsigned int)(d_aperture * 1000), 1000 );
    
    // Convert aperture to APEX
    double aperture_apex = log(d_aperture) / log(sqrt(2.0));
    
    x->aperture     = unsigned_rational_construct( (unsigned int)(aperture_apex * 1000), 1000 );    
    
    x->focal_length = unsigned_rational_construct(3, 1);
    
    x->digital_zoom = unsigned_rational_construct(1, 1);
    
    x->metering_mode = gpr_metering_mode_center_weighted_average;
    
    x->focal_length_in_35mm_film = 15;
    
    x->exposure_program = gpr_exposure_program_normal;
    
    x->light_source = gpr_light_source_auto;
    
    x->flash = gpr_flash_not_supported;
    
    x->sensing_method = gpr_sensing_method_chip_color_area;
    
    x->file_source = gpr_file_source_digital_still;
    
    x->scene_type = gpr_scene_type_directly_photographed;
    
    x->white_balance = gpr_white_balance_auto;
    
    x->exposure_mode = gpr_exposure_mode_auto;
    
    x->scene_capture_type = gpr_scene_capture_type_standard;
    
    x->gain_control = gpr_gain_control_normal;
    
    x->contrast = gpr_contrast_normal;
    
    x->saturation = gpr_saturation_normal;
    
    x->sharpness = gpr_sharpness_hard;
    
    x->iso_speed_rating = 232;
    
    x->date_time_original = construct_dng_date_and_time (2016, 03, 25, 15, 55, 23 );
    
    x->date_time_digitized = x->date_time_original;
    
    strcpy( x->camera_make, "GoPro" );

    strcpy( x->camera_model, "HERO6 Black" );
    
    sprintf( x->software_version, "%d.%d.%d", GPR_VERSION_MAJOR, GPR_VERSION_MINOR, GPR_VERSION_REVISION );

    strcpy( x->user_comment, "" );
}

void gpr_exif_info_get_camera_make_and_model( const gpr_exif_info* x, char camera_make_and_model[256] )
{
    strcpy(camera_make_and_model, x->camera_make );
    strcpy(camera_make_and_model + strlen(camera_make_and_model), " " );
    strcpy(camera_make_and_model + strlen(camera_make_and_model), x->camera_model );
}

