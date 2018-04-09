
#include "gpr_profile_info.h"
#include "stdc_includes.h"

void gpr_profile_info_set_defaults(gpr_profile_info* x)
{
    x->compute_color_matrix = true;
    
    x->matrix_weighting = 1.0;

    x->illuminant1 = 3;     // tungsten
    x->illuminant2 = 23;    // d50
    
    x->wb1[0] = 1.339600;
    x->wb1[1] = 1.00000;
    x->wb1[2] = 2.780029;
    
    x->wb2[0] = 1.9036;
    x->wb2[1] = 1.00000;
    x->wb2[2] = 1.7483;
    
    {
        const double cam_to_srgb[] = { 1.2963, -0.2025, -0.0939, -0.4789, 1.5728, -0.0939, -0.1007, -0.7605, 1.8612 };
        
        x->cam_to_srgb_1[0][0] = cam_to_srgb[0];
        x->cam_to_srgb_1[0][1] = cam_to_srgb[1];
        x->cam_to_srgb_1[0][2] = cam_to_srgb[2];
        
        x->cam_to_srgb_1[1][0] = cam_to_srgb[3];
        x->cam_to_srgb_1[1][1] = cam_to_srgb[4];
        x->cam_to_srgb_1[1][2] = cam_to_srgb[5];
        
        x->cam_to_srgb_1[2][0] = cam_to_srgb[6];
        x->cam_to_srgb_1[2][1] = cam_to_srgb[7];
        x->cam_to_srgb_1[2][2] = cam_to_srgb[8];
    }
    
    {
        const double cam_to_srgb[] = { 1.5580, -0.3019, -0.2561, -0.3023, 1.6328, -0.3305, -0.0365, -0.5127, 1.5492 }; 
        
        x->cam_to_srgb_2[0][0] = cam_to_srgb[0];
        x->cam_to_srgb_2[0][1] = cam_to_srgb[1];
        x->cam_to_srgb_2[0][2] = cam_to_srgb[2];
        
        x->cam_to_srgb_2[1][0] = cam_to_srgb[3];
        x->cam_to_srgb_2[1][1] = cam_to_srgb[4];
        x->cam_to_srgb_2[1][2] = cam_to_srgb[5];
        
        x->cam_to_srgb_2[2][0] = cam_to_srgb[6];
        x->cam_to_srgb_2[2][1] = cam_to_srgb[7];
        x->cam_to_srgb_2[2][2] = cam_to_srgb[8];
    }
    
    memset( x->color_matrix_1, 0, sizeof(x->color_matrix_1) );
    memset( x->color_matrix_2, 0, sizeof(x->color_matrix_2) );
}

