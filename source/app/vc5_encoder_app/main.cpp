/*! @file main.cpp
 *
 *  @brief Main program file for the sample vc5 encoder.
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

#include "vc5_encoder.h"

#include "argument_parser.h"
#include "timer.h"
#include "gpr_buffer.h"
#include "md5.h"
#include "log.h"
#include "logcurve.h"
#include "common_app_def.h"

using namespace std;

class my_argument_parser : public argument_parser
{
protected:
    
    bool    help;
    
    bool    verbose;
    
public:
    
    bool    validate;
    
    bool    dump_info;
    
    int     input_width;
    
    int     input_height;
    
    int     input_pitch;
    
    int     input_skip_rows;
    
    string  log_curve_file_path;
    
    string  input_pixel_format;
    
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
        
        ("InputWidth,w",                                    input_width,                                4000,                   "Input image width in pixel samples e.g. 4000")
        
        ("InputHeight,h",                                   input_height,                               3000,                   "Input image height in pixel samples e.g. 3000")
        
        ("InputPitch,p",                                    input_pitch,                                -1,                     "Input image pitch in bytes e.g. 8000")
        
        ("InputPixelFormat,x",                              input_pixel_format,                         string("rggb14"),       "Input pixel format [rggb12, rggb12p, rggb14, gbrg12, gbrg12p]")
        
        ("OutputFilePath,o",                                output_file_path,                           string(""),             "Output file path")
        
        ("PrintLogCurve,l",                                 log_curve_file_path,                        string(""),             "File for encoding log curve output");
        ;
    }
};

/*!
	@brief Main entry point for the reference encoder

	Usage: encoder [options] input output
	
	The input argument is the pathname to a file that contains a single image that
	is the input the image unpacking process (see @ref ImageUnpackingProcess). The output argument is
    the pathname to a file that will contain the encoded bitstream_file.  Media containers are not
    currently supported by the reference encoder.  The command-line options are described in @ref ParseParameters.
*/
int main(int argc, char *argv[])
{
    my_argument_parser args;

    LogInit();

    char line[MAX_STDOUT_LINE];
    sprintf( line, "VC5 Encoder Version %d.%d.%d [%s @ %s] ", VC5_VERSION_MAJOR, VC5_VERSION_MINOR, VC5_VERSION_REVISION, GIT_BRANCH, GIT_COMMIT_HASH );

    if( args.parse(argc, argv, line, "[0000000000]") )
        return -1;

    int encoder_run;
    int encoder_run_count = 1;
    
	CODEC_ERROR error = CODEC_ERROR_OKAY;

    vc5_encoder_parameters vc5_encoder_params;
    vc5_encoder_parameters_set_default(&vc5_encoder_params);
    
    vc5_encoder_params.enabled_parts    = VC5_ENABLED_PARTS;
    vc5_encoder_params.input_width      = args.input_width;
    vc5_encoder_params.input_height     = args.input_height;
    vc5_encoder_params.input_pitch      = args.input_pitch;
    vc5_encoder_params.mem_alloc        = malloc;
    vc5_encoder_params.mem_free         = free;

    if( strcmp(args.input_pixel_format.c_str(), "rggb12") == 0 )
    {
        vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_12;
        
        if( args.input_pitch == -1 )
            vc5_encoder_params.input_pitch = args.input_width * 2;
    }
    if( strcmp(args.input_pixel_format.c_str(), "rggb12p") == 0 )
    {
        vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_12P;
        
        if( args.input_pitch == -1 )
            vc5_encoder_params.input_pitch = (args.input_width * 3 / 4) * 2;
    }
    else if( strcmp(args.input_pixel_format.c_str(), "rggb14") == 0 )
    {
        vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_RGGB_14;

        if( args.input_pitch == -1 )
            args.input_pitch = args.input_width * 2;
    }
    else if( strcmp(args.input_pixel_format.c_str(), "gbrg12") == 0 )
    {
        vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_GBRG_12;

        if( args.input_pitch == -1 )
            args.input_pitch = args.input_width * 2;
    }
    else if( strcmp(args.input_pixel_format.c_str(), "gbrg12p") == 0 )
    {
        vc5_encoder_params.pixel_format = VC5_ENCODER_PIXEL_FORMAT_GBRG_12P;

        if( args.input_pitch == -1 )
            args.input_pitch = (args.input_width * 3 / 4) * 2;
    }
    else
    {
        LogPrint("Invalid input format: %s", args.input_pixel_format.c_str());
        return -1;
    }
    
    gpr_buffer raw_image = { NULL, 0  };
    
	// Print the flags indicating which parts are enabled for this encoder
	LogPrint("Raw Input image: %s", args.input_file_path.c_str() );
	LogPrint("Vc5 Output file: %s", args.output_file_path.c_str() );

    if( read_from_file( &raw_image, args.input_file_path.c_str(), vc5_encoder_params.mem_alloc, vc5_encoder_params.mem_free ) )
    {
        LogPrint("Could not read input file: %s", args.input_file_path.c_str());
        return -1;
    }
    
    TIMER timer;
    InitTimer(&timer);

    unsigned char old_digest[MD5_DIGEST_SIZE];
    
    for (encoder_run = 0; encoder_run < encoder_run_count; encoder_run++)
    {
        gpr_buffer vc5_image = { NULL, 0  };
        
        StartTimer(&timer);
        
        vc5_encoder_process( &vc5_encoder_params, &raw_image, &vc5_image, NULL );
        
        StopTimer(&timer);
        
        assert( vc5_image.buffer && vc5_image.size > 0 );

        if( write_to_file( &vc5_image, args.output_file_path.c_str() ) )
        {
            LogPrint("Error writing bitstream to location %s", args.output_file_path.c_str() );
            return -1;
        }

        {
            static const char* hex = "0123456789ABCDEF";
            
            unsigned char digest[MD5_DIGEST_SIZE];
            context_md5_t ctx;
            
            MD5Init(&ctx);
            MD5Update(&ctx, (unsigned char *)vc5_image.buffer, vc5_image.size );
            MD5Final(digest, &ctx);
            
            {
                int i;
                unsigned char logged_digest[MD5_DIGEST_SIZE * 2 + 1];
                
                for (i = 0; i < MD5_DIGEST_SIZE; i++)
                {
                    logged_digest[i * 2 + 0] = hex[ digest[i] >> 4 ];
                    logged_digest[i * 2 + 1] = hex[ digest[i] & 0xf ];
                }
                
                logged_digest[i * 2] = '\0';

                LogPrint("%d %s", encoder_run, logged_digest);
            }
            
            if( encoder_run > 0 )
            {
                if( memcmp(old_digest, digest, sizeof(digest) ) )
                {
                    LogPrint("ERROR digests in run %d and %d do not match", encoder_run, encoder_run - 1 );
                    return -1;
                }
            }
            
            memcpy(old_digest, digest, sizeof(digest));
        }
        
        vc5_encoder_params.mem_free(vc5_image.buffer);
    }

    LogPrint("Encoding %.3f secs per frame", TimeSecs(&timer) / encoder_run_count );
    
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
            file << EncoderLogCurve[i];
            
            file << endl;
        }
        
        file.close();
    }
    
    if( raw_image.buffer )
    {
        vc5_encoder_params.mem_free( raw_image.buffer );
    }
    
    LogUninit();
    
	return error;
}
