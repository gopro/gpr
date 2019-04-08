/*! @file main.cpp
 *
 *  @brief Main program file for the gpr_tools.
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

#include <stdio.h>
#include <string.h>

#include "argument_parser.h"

#include "gpr.h"
#include "gpr_buffer.h"
#include "gpr_print_utils.h"
#include "main_c.h"

#include "common_app_def.h"

using namespace std;

class my_argument_parser : public argument_parser
{
protected:
    
    bool    help;
    
    bool    verbose;

public:
    
    bool    dump_gpr_parameters;
    
    string  jpg_preview_file_path;

    int     jpg_preview_file_width;
    
    int     jpg_preview_file_height;

    int     input_width;
    
    int     input_height;

    int     input_pitch;
    
    int     input_skip_rows;

	int		input_header_skip;
    
    string  input_pixel_format;
    
    string  input_file_path;
    
    string  gpmf_file_path;

    string  rgb_file_resolution;
    
    int     rgb_file_bits;
    
    string  output_file_path;

    string  apply_gpr_parameters;
    
public:

    bool get_verbose() { return verbose; }
    
    bool get_help() { return help; }
    
    void set_options()
    {
        command_options.addOptions()
        /* long and short name */                           /* variable to update */                    /* default value */     /* help text */
        ("help",                                            help,                                       false,                  "Prints this help text")
        
        ("verbose",                                         verbose,                                    false,                  "Verbosity of the output")

        ("JpgPreviewFilePath,P",                            jpg_preview_file_path,                      string(""),             "Preview jpg file path")
        ("JpgPreviewFileWidth,W",                           jpg_preview_file_width,                     0,                      "Preview jpg file width")
        ("JpgPreviewFileHeight,H",                          jpg_preview_file_height,                    0,                      "Preview jpg file height")
        
        ("DumpGprParameters,d",                             dump_gpr_parameters,                        false,                  "Dump GPR parameters to standard output")

        ("InputSkipRows,s",                                 input_skip_rows,                            0,                      "Input image rows to skip")

        ("InputFilePath,i",                                 input_file_path,                            string(""),             "Input file path.\n(files types: GPR, DNG, RAW)")
        
        ("InputWidth,w",                                    input_width,                                4000,                   "Input image width in pixel samples [4000]")
        
        ("InputHeight,h",                                   input_height,                               3000,                   "Input image height in pixel samples [3000]")

		("InputPitch,p",									input_pitch,								8000,					"Input image pitch in bytes [8000]")

		("InputHeaderSkip,b",								input_header_skip,							-1,						"Skip any raw header in bytes e.g. 256")

        ("InputPixelFormat,x",                              input_pixel_format,                         string("rggb14"),       "Input pixel format \n(rggb12, rggb12p, [rggb14], gbrg12, gbrg12p)")
        
        ("ApplyGprParameters,a",                            apply_gpr_parameters,                       string(""),             "Parameters to use for GPR or DNG file.")
        
        ("GPMFFilePath,g",                                  gpmf_file_path,                             string(""),             "GPMF file path")

        ("RgbFileResolution,r",                             rgb_file_resolution,                        string(""),             "Output RGB resolution \n[1:1, 2:1, 4:1, 8:1. 16:1]")
        ("RgbFileBits,b",                                   rgb_file_bits,                              8,                      "Output RGB bits [8]")
        
        ("OutputFilePath,o",                                output_file_path,                           string(""),             "Output file path.\n(files types: GPR, DNG, PPM, RAW, JPG)");
        ;
    }
};

int dng_dump(const char*  input_file_path)
{
    gpr_allocator allocator;
    allocator.Alloc = malloc;
    allocator.Free = free;
    
    gpr_buffer input_buffer  = { NULL, 0 };
    
    gpr_parameters params;
    
    gpr_parameters_set_defaults(&params);
    
    if( read_from_file( &input_buffer, input_file_path, allocator.Alloc, allocator.Free ) != 0 )
    {
        return -1;
    }
    
    int success = gpr_parse_metadata( &allocator, &input_buffer, &params );
    
    if( success )
    {
        gpr_parameters_print( &params, NULL );
    }
    
    return 0;
}

int main(int argc, char *argv [])
{
    my_argument_parser args;
    
    
    char zerotag[MAX_STDOUT_LINE];
    sprintf(zerotag, "[%5d-ms] ", 0);

    char line[MAX_STDOUT_LINE];
    sprintf( line, "GPR Tools Version %d.%d.%d [%s @ %s] ", GPR_VERSION_MAJOR, GPR_VERSION_MINOR, GPR_VERSION_REVISION, GIT_BRANCH, GIT_COMMIT_HASH );
    
    if( argc == 1 || args.parse(argc, argv, line, zerotag) )
    {
        printf("\n");
        printf("-- Example Commnads (please see data/tests/run_tests.sh for more examples) --\n");
        printf("GPR to DNG: \n");
        printf("  %s -i ./data/samples/Hero6/GOPR0024.GPR -o ./data/samples/Hero6/GOPR0024.DNG \n\n", argv[0] );
        printf("GPR to RGB (PPM format in 1000x750 resolution): \n");
        printf("  %s -i ./data/samples/Hero6/GOPR0024.GPR -o ./data/samples/Hero6/GOPR0024.PPM -r 4:1 \n\n", argv[0] );
        printf("GPR to RGB (JPG format in 500x375 resolution): \n");
        printf("  %s -i ./data/samples/Hero6/GOPR0024.GPR -o ./data/samples/Hero6/GOPR0024.JPG -r 8:1 \n\n", argv[0] );
        printf("Analyze a GPR or DNG file and output metadata parameters to a file: \n");
        printf("  %s -i ./data/samples/Hero6/GOPR0024.GPR -d 1 > ./data/samples/Hero6/GOPR0024.TXT \n\n", argv[0] );
        printf("Read RAW pixel data, along with gpr parameters (from a file) and apply to an output GPR or DNG file: \n");
        printf("  %s -i ./data/samples/Hero6/GOPR0024.RAW -o ./data/samples/Hero6/GOPR0024.DNG -a ./data/samples/Hero6/GOPR0024.TXT \n\n", argv[0] );

        return -1;
    }
    
    if( args.dump_gpr_parameters )
    {
        if( dng_dump(args.input_file_path.c_str()) != 0 )
            return -1;
    }
    else
    {
        string ext = strrchr( args.input_file_path.c_str(),'.');
        
        if( args.output_file_path == string("") && ( ext == string(".GPR") || ext == string(".gpr") ) )
        {
            args.output_file_path = args.input_file_path;
            args.output_file_path.erase(args.output_file_path.find_last_of("."), string::npos);
            
            args.output_file_path = args.output_file_path + string(".DNG");
        }
    }
    
    fprintf( stderr, "%s Input File: %s \n",    zerotag, args.input_file_path.c_str() );
    fprintf( stderr, "%s Output File: %s \n",   zerotag, args.output_file_path.c_str() );

    if( args.output_file_path != "" )
    {
        return dng_convert_main(args.input_file_path.c_str(), args.input_width, args.input_height, args.input_pitch, args.input_skip_rows, args.input_header_skip, args.input_pixel_format.c_str(),
                                args.output_file_path.c_str(), args.apply_gpr_parameters.c_str(), args.gpmf_file_path.c_str(), args.rgb_file_resolution.c_str(), args.rgb_file_bits,
                                args.jpg_preview_file_path.c_str(), args.jpg_preview_file_width, args.jpg_preview_file_height );
    }
    
    return 0;
}
    
