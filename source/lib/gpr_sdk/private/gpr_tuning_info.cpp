
#include "gpr_buffer_auto.h"

#include "gpr_tuning_info.h"

static void _static_black_level_set_defaults(gpr_static_black_level* x)
{
    const int black = 0;
    
    x->r_black      = black;
    x->g_r_black    = black;
    x->g_b_black    = black;
    x->b_black      = black;
}

static void _ae_info_set_defaults(gpr_auto_exposure_info* x)
{
    x->iso_value    = 228;
    x->shutter_time = 34952;
}

static void _dgain_saturation_level_set_defaults(gpr_saturation_level* x)
{
    const int32_t saturation_level = 16383;
    
    x->level_red        = saturation_level;
    x->level_green_even = saturation_level;
    x->level_green_odd  = saturation_level;
    x->level_blue       = saturation_level;
}

static void _wb_gains_set_defaults(gpr_white_balance_gains* x)
{
    x->r_gain   = (float_t)6273.0 / 4096.0;
    x->g_gain   = (float_t)4096.0 / 4096.0;
    x->b_gain   = (float_t)8371.0 / 4096.0;
}

static void _gain_map_set_defaults( gpr_tuning_info* tuning_info )
{
    tuning_info->gain_map.size = 0;
    
    tuning_info->gain_map.buffers[0] = NULL;
    tuning_info->gain_map.buffers[1] = NULL;
    tuning_info->gain_map.buffers[2] = NULL;
    tuning_info->gain_map.buffers[3] = NULL;
}

int32_t gpr_tuning_info_get_dgain_saturation_level(const gpr_tuning_info* x, GPR_RAW_CHANNEL channel)
{
    switch(channel)
    {
        case RAW_CHANNEL_RED:
            return x->dgain_saturation_level.level_red;
        case RAW_CHANNEL_GREEN_EVEN:
            return x->dgain_saturation_level.level_green_even;
        case RAW_CHANNEL_GREEN_ODD:
            return x->dgain_saturation_level.level_green_odd;
        case RAW_CHANNEL_BLUE:
            return x->dgain_saturation_level.level_blue;
        default:
            assert(0);
            return 0;
    }
}

void gpr_tuning_info_set_defaults( gpr_tuning_info* x )
{
    x->orientation = ORIENTATION_DEFAULT;
    
    _static_black_level_set_defaults(&x->static_black_level);

    _dgain_saturation_level_set_defaults(&x->dgain_saturation_level);
    
    _wb_gains_set_defaults(&x->wb_gains);

    _ae_info_set_defaults(&x->ae_info);
    
    _gain_map_set_defaults( x );
    
    x->pixel_format = PIXEL_FORMAT_RGGB_14;
}


