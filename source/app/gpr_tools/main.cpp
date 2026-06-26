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
    
    bool    print_gpr_json;
    
    string  preview_file_path;

    int     preview_file_width;
    
    int     preview_file_height;

    int     input_width;
    
    int     input_height;

    int     input_pitch;
    
    int     input_skip_rows;
    
    string  input_pixel_format;
    
    string  input_file_path;
    
    string  gpmf_file_path;

    string  rgb_file_resolution;
    
    int     rgb_file_bits;
    
    string  output_file_path;

    string  apply_gpr_json;
    
public:

    bool get_verbose() { return verbose; }
    
    bool get_help() { return help; }
    
    void set_options()
    {
        command_options.addOptions()
        /* long and short name */                           /* variable to update */                    /* default value */     /* help text */
        ("help,h",                                          help,                                       false,                  "Prints this help text")
        
        ("verbose,v",                                       verbose,                                    false,                  "Verbosity of the output")

        ("preview_file_path",                               preview_file_path,                          string(""),             "Preview (jpg) file path")
        ("preview_file_width",                              preview_file_width,                         0,                      "Preview (jpg) file width")
        ("preview_file_height",                             preview_file_height,                        0,                      "Preview (jpg) file height")
        
        ("print_gpr_json,d",                                print_gpr_json,                             false,                  "Print gpr params (as json) to standard output")
        ("apply_gpr_json,a",                                apply_gpr_json,                             string(""),             "Use gpr params for dng metadata")

        ("input_file_path,i",                               input_file_path,                            string(""),             "Input file path.\n(files types: GPR, DNG, RAW)")
        ("input_width,w",                                   input_width,                                4000,                   "Input image width in pixel samples [4000]")
        ("input_height,h",                                  input_height,                               3000,                   "Input image height in pixel samples [3000]")
        ("input_pitch,p",                                   input_pitch,                                8000,                   "Input image pitch in bytes [8000]")
        ("input_pixel_format,x",                            input_pixel_format,                         string("rggb14"),       "Input pixel format \n(rggb12, rggb12p, [rggb14], gbrg12, gbrg12p)")
        ("input_skip_rows,s",                               input_skip_rows,                            0,                      "Input image rows to skip")

        ("output_file_path,o",                              output_file_path,                           string(""),             "Output file path.\n(files types: GPR, DNG, PPM, RAW, JPG)")

        ("gpmf_file_path,g",                                gpmf_file_path,                             string(""),             "GPMF file path")

        ("rgb_file_resolution,r",                           rgb_file_resolution,                        string(""),             "Output RGB resolution \n[1:1, 2:1, [4:1], 8:1. 16:1]")
        ("rgb_file_bits,b",                                 rgb_file_bits,                              8,                      "Output RGB bits [8]");
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
    
    if( args.parse(argc, argv, line, zerotag) )
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
    
    if( args.print_gpr_json )
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
        return dng_convert_main(args.input_file_path.c_str(), args.input_width, args.input_height, args.input_pitch, args.input_skip_rows, args.input_pixel_format.c_str(),
                                args.output_file_path.c_str(), args.apply_gpr_json.c_str(), args.gpmf_file_path.c_str(), args.rgb_file_resolution.c_str(), args.rgb_file_bits,
                                args.preview_file_path.c_str(), args.preview_file_width, args.preview_file_height );
    }
    
    return 0;
}
    
