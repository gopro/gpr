/*! @file main.cpp
 *
 *  @brief Main program file for the sample vc5 decoder.
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

#include "stdc_includes.h"
#include "stdcpp_includes.h"

#if GPR_READING
#include "vc5_decoder.h"
#endif

#include "argument_parser.h"
#include "timer.h"
#include "gpr_buffer.h"
#include "log.h"
#include "logcurve.h"
#include "common_app_def.h"

#define DECODER_RUN_COUNT 1

using namespace std;

class my_argument_parser : public argument_parser
{
protected:
    
    bool    help;
    
    bool    verbose;
    
public:
    
    string  log_curve_file_path;
    
    string  output_pixel_format;
    
    string  input_file_path;
    
    string  output_file_path;
    
public:
    
    bool get_verbose() { return verbose; }
    
    bool get_help() { return help; }
    
    void set_options()
    {
        command_options.addOptions()
        /* long and short name */                           /* variable to update */                    /* default value */     /* help text */
        ("help",                                            help,                                       false,                  "Prints this help text")
        ("verbose",                                         verbose,                                    false,                  "Verbosity of the output")
        
        ("InputFilePath,i",                                 input_file_path,                            string(""),             "Input file path")
        
        ("OutputPixelFormat,x",                             output_pixel_format,                        string("rggb14"),       "Output pixel format [rggb12, rggb12p, rggb14, gbrg12, gbrg12p]")
        
        ("OutputFilePath,o",                                output_file_path,                           string(""),             "Output file path")
        
        ("PrintLogCurve,l",                                 log_curve_file_path,                        string(""),             "File for encoding log curve output");
        ;
    }
};

/*!
	@brief Main entry point for the reference decoder

	The program takes two arguments: the pathname to the file that contains a
	sample to decode and the pathname to the output file for the decoded image.

	The input file should contain a single encoded sample without any header.

	The output file can be a DPX file, otherwise the decoded image is written to the output
	file without a header.

	The image is decoded to the same dimensions as the encoded image and the decoded format
	is the same format as the original source image input to the encoder.
*/
int main(int argc, char *argv[])
{
    my_argument_parser args;
    
    char line[MAX_STDOUT_LINE];
    sprintf( line, "VC5 Decoder Version %d.%d.%d [%s @ %s] ", VC5_VERSION_MAJOR, VC5_VERSION_MINOR, VC5_VERSION_REVISION, GIT_BRANCH, GIT_COMMIT_HASH );

    if( args.parse(argc, argv, line, "[0000000000]" ) )
        return -1;

    int i;

	CODEC_ERROR error = CODEC_ERROR_OKAY;
    
    vc5_decoder_parameters vc5_decoder_params;
    vc5_decoder_parameters_set_default(&vc5_decoder_params);
    vc5_decoder_params.enabled_parts = VC5_ENABLED_PARTS;
    vc5_decoder_params.mem_alloc     = malloc;
    vc5_decoder_params.mem_free      = free;

    if( strcmp(args.output_pixel_format.c_str(), "rggb12") == 0 )
    {
        vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_RGGB_12;
    }
    else if( strcmp(args.output_pixel_format.c_str(), "rggb14") == 0 )
    {
        vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_RGGB_14;
    }
    else if( strcmp(args.output_pixel_format.c_str(), "gbrg12") == 0 )
    {
        vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_GBRG_12;
    }
    else if( strcmp(args.output_pixel_format.c_str(), "gbrg14") == 0 )
    {
        vc5_decoder_params.pixel_format = VC5_DECODER_PIXEL_FORMAT_GBRG_14;
    }
    else
    {
        LogPrint("Invalid output format: %s", args.output_pixel_format.c_str());
        return -1;
    }

    LogInit();

    gpr_buffer vc5_image = { NULL, 0  };

    // Print the flags indicating which parts are enabled for this encoder
    LogPrint("Vc5 Input image: %s", args.input_file_path.c_str() );
    LogPrint("Raw Output file: %s", args.output_file_path.c_str() );
    
    if( read_from_file( &vc5_image, args.input_file_path.c_str(), vc5_decoder_params.mem_alloc, vc5_decoder_params.mem_free ) )
    {
        LogPrint("Could not read input file: %s", args.input_file_path.c_str());
        exit(-1);
    }
    
    TIMER timer; // Performance timer
    InitTimer(&timer);

    for (i = 0; i < DECODER_RUN_COUNT; i++)
    { // Decode all frames

        gpr_buffer raw_image = { NULL, 0  };
        gpr_rgb_buffer rgb_image = { NULL, 0, 0, 0  };

        StartTimer(&timer);
        
        LogPrint("%d ", i);
        
        vc5_decoder_process( &vc5_decoder_params, &vc5_image, &raw_image, &rgb_image );
        
        StopTimer(&timer);
        
        fflush(stdout);

        assert( raw_image.buffer && raw_image.size > 0 );

        if( write_to_file( &raw_image, args.output_file_path.c_str() ) )
        {
            LogPrint("Error writing bitstream to location %s", args.output_file_path.c_str() );
            return -1;
        }
        
        if( raw_image.buffer )
        {
            vc5_decoder_params.mem_free(raw_image.buffer);
        }
        
        if( rgb_image.buffer )
        {
            vc5_decoder_params.mem_free(rgb_image.buffer);
        }
    }
    
    LogPrint("Decoding %.3f secs per frame", TimeSecs(&timer) / DECODER_RUN_COUNT );
    
    if( args.log_curve_file_path != "" )
    {
        LogPrint("Printing log curve to %s", args.log_curve_file_path.c_str() );
        
        ofstream file;
        file.open ( args.log_curve_file_path.c_str() );
        
        
        for( int i = 0; i < LOG_CURVE_TABLE_LENGTH; i++ )
        {
            file.fill( '0' );
            file.width( 4 );
            file << i;
            
            file << ": ";
            
            file.fill( '0' );
            file.width( 4 );
            file << (DecoderLogCurve[i] >> 4);
            
            file << endl;
        }
        
        file.close();
    }
    
    if( vc5_image.buffer )
    {
        vc5_decoder_params.mem_free( vc5_image.buffer );
    }

    LogUninit();

	return error;
}
