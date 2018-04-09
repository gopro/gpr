#!/usr/bin/python

# --- Initialization - setup paths
import sys
import os
sys.path.append( os.path.normpath(os.path.join(__file__, "..")) + '/../../SCRIPTS' )

from commonlib import *

gyp_build_object = {

    "targets": [
        
        {
            "target_name": "dng_sdk",
            
            "type": "static_library",
            
            'include_dirs': [
            ],

            'defines': [ '<@(default_defines)', 'qDNGXMPFiles=0', 'qDNGXMPDocOps=0' ],

            "includes": [
                "../xmp_sdk/xmp_sdk.gypi",
            ],

            "sources": [
            
                "./dng_validate.cpp" ,

                "./dng_1d_function.cpp" ,
                "./dng_1d_function.h" ,
                "./dng_1d_table.cpp" ,
                "./dng_1d_table.h" ,
                "./dng_abort_sniffer.cpp" ,
                "./dng_abort_sniffer.h" ,
                "./dng_area_task.cpp" ,
                "./dng_area_task.h" ,
                "./dng_assertions.h" ,
                "./dng_auto_ptr.h" ,
                "./dng_bad_pixels.cpp" ,
                "./dng_bad_pixels.h" ,
                "./dng_bottlenecks.cpp" ,
                "./dng_bottlenecks.h" ,
                "./dng_camera_profile.cpp" ,
                "./dng_camera_profile.h" ,
                "./dng_classes.h" ,
                "./dng_color_space.cpp" ,
                "./dng_color_space.h" ,
                "./dng_color_spec.cpp" ,
                "./dng_color_spec.h" ,
                "./dng_date_time.cpp" ,
                "./dng_date_time.h" ,
                "./dng_errors.h" ,
                "./dng_exceptions.cpp" ,
                "./dng_exceptions.h" ,
                "./dng_exif.cpp" ,
                "./dng_exif.h" ,
                "./dng_fast_module.h" ,
                "./dng_file_stream.cpp" ,
                "./dng_file_stream.h" ,
                "./dng_filter_task.cpp" ,
                "./dng_filter_task.h" ,
                "./dng_fingerprint.cpp" ,
                "./dng_fingerprint.h" ,
                "./dng_flags.h" ,
                "./dng_gain_map.cpp" ,
                "./dng_gain_map.h" ,
                "./dng_globals.cpp" ,
                "./dng_globals.h" ,
                "./dng_host.cpp" ,
                "./dng_host.h" ,
                "./dng_hue_sat_map.cpp" ,
                "./dng_hue_sat_map.h" ,
                "./dng_ifd.cpp" ,
                "./dng_ifd.h" ,
                "./dng_image.cpp" ,
                "./dng_image.h" ,
                "./dng_image_writer.cpp" ,
                "./dng_image_writer.h" ,
                "./dng_info.cpp" ,
                "./dng_info.h" ,
                "./dng_iptc.cpp" ,
                "./dng_iptc.h" ,
                "./dng_jpeg_image.cpp" ,
                "./dng_jpeg_image.h" ,
                "./dng_lens_correction.cpp" ,
                "./dng_lens_correction.h" ,
                "./dng_linearization_info.cpp" ,
                "./dng_linearization_info.h" ,
                "./dng_lossless_jpeg.cpp" ,
                "./dng_lossless_jpeg.h" ,
                "./dng_matrix.cpp" ,
                "./dng_matrix.h" ,
                "./dng_memory.cpp" ,
                "./dng_memory.h" ,
                "./dng_memory_stream.cpp" ,
                "./dng_memory_stream.h" ,
                "./dng_misc_opcodes.cpp" ,
                "./dng_misc_opcodes.h" ,
                "./dng_mosaic_info.cpp" ,
                "./dng_mosaic_info.h" ,
                "./dng_mutex.cpp" ,
                "./dng_mutex.h" ,
                "./dng_negative.cpp" ,
                "./dng_negative.h" ,
                "./dng_opcode_list.cpp" ,
                "./dng_opcode_list.h" ,
                "./dng_opcodes.cpp" ,
                "./dng_opcodes.h" ,
                "./dng_orientation.cpp" ,
                "./dng_orientation.h" ,
                "./dng_parse_utils.cpp" ,
                "./dng_parse_utils.h" ,
                "./dng_pixel_buffer.cpp" ,
                "./dng_pixel_buffer.h" ,
                "./dng_point.cpp" ,
                "./dng_point.h" ,
                "./dng_preview.cpp" ,
                "./dng_preview.h" ,
                "./dng_pthread.cpp" ,
                "./dng_pthread.h" ,
                "./dng_rational.cpp" ,
                "./dng_rational.h" ,
                "./dng_read_image.cpp" ,
                "./dng_read_image.h" ,
                "./dng_rect.cpp" ,
                "./dng_rect.h" ,
                "./dng_ref_counted_block.cpp" ,
                "./dng_ref_counted_block.h" ,
                "./dng_reference.cpp" ,
                "./dng_reference.h" ,
                "./dng_render.cpp" ,
                "./dng_render.h" ,
                "./dng_resample.cpp" ,
                "./dng_resample.h" ,
                "./dng_sdk_limits.h" ,
                "./dng_shared.cpp" ,
                "./dng_shared.h" ,
                "./dng_simple_image.cpp" ,
                "./dng_simple_image.h" ,
                "./dng_spline.cpp" ,
                "./dng_spline.h" ,
                "./dng_stream.cpp" ,
                "./dng_stream.h" ,
                "./dng_string.cpp" ,
                "./dng_string.h" ,
                "./dng_string_list.cpp" ,
                "./dng_string_list.h" ,
                "./dng_tag_codes.h" ,
                "./dng_tag_types.cpp" ,
                "./dng_tag_types.h" ,
                "./dng_tag_values.h" ,
                "./dng_temperature.cpp" ,
                "./dng_temperature.h" ,
                "./dng_tile_iterator.cpp" ,
                "./dng_tile_iterator.h" ,
                "./dng_tone_curve.cpp" ,
                "./dng_tone_curve.h" ,
                "./dng_types.h" ,
                "./dng_uncopyable.h" ,
                "./dng_utils.cpp" ,
                "./dng_utils.h" ,
                "./dng_xmp.cpp" ,
                "./dng_xmp.h" ,
                "./dng_xmp_sdk.cpp" ,
                "./dng_xmp_sdk.h" ,
                "./dng_xy_coord.cpp" ,
                "./dng_xy_coord.h" ,
            ]
        }
    ]
}

from argparse import ArgumentParser
parser = ArgumentParser(conflict_handler='resolve')
parser.add_argument('-m',	'--modules',       			default="all",  help='Modules to include in build step', 					choices=["gpr_encoding", "gpr_decoding", "all"], )

args = parser.parse_args()

if args.modules == "gpr_encoding" or args.modules == "all":
	gyp_build_object['targets'][0]['defines'].append( "ENABLE_GPR_ENCODING=1" )
else:    
    gyp_build_object['targets'][0]['defines'].append( "ENABLE_GPR_ENCODING=0" )

if args.modules == "gpr_decoding" or args.modules == "all":
	gyp_build_object['targets'][0]['defines'].append( "ENABLE_GPR_DECODING=1" )
else:
    gyp_build_object['targets'][0]['defines'].append( "ENABLE_GPR_DECODING=0" )

print( gyp_build_object )