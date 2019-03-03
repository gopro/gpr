/*! @file argument_parser.cpp
 *
 *  @brief Implement class to handle argument parsing
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

#include "argument_parser.h"

#include <stdio.h>

using namespace std;

#ifdef __GNUC__
#define COMPILER  "[GCC %d.%d.%d]", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__
#elif __INTEL_COMPILER
#define COMPILER  "[ICC %d]", __INTEL_COMPILER
#elif  _MSC_VER
#define COMPILER  "[VS %d]", _MSC_VER
#else
#define COMPILER "[Unknown-CXX]"
#endif

#ifdef _WIN32
#define OPERATING_SYSTEM        "[Windows]"
#elif  __linux
#define OPERATING_SYSTEM        "[Linux]"
#elif  __CYGWIN__
#define OPERATING_SYSTEM        "[Cygwin]"
#elif __APPLE__
#define OPERATING_SYSTEM        "[Mac OS X]"
#else
#define OPERATING_SYSTEM        "[Unknown-OS]"
#endif

#define NUMBER_OF_BITS "[%d bit] ", (sizeof(void*) == 8 ? 64 : 32) ///< used for checking 64-bit O/S

argument_parser::argument_parser(bool verbose)
{
}

void argument_parser::set_options()
{
}

int argument_parser::parse(int argc, char *argv [], const char* application_text, const char* prefix_text)
{
    argument_count = argc;
    
    for (int i = 0; i < argument_count; i++)
        arguments[i] = argv[i];
    
    set_options();
    
    program_options_lite::setDefaults(command_options);
    
    const list<const char*>& argv_unhandled = program_options_lite::scanArgv(command_options, argument_count, (const char**) arguments);
    
    for (list<const char*>::const_iterator it = argv_unhandled.begin(); it != argv_unhandled.end(); it++)
    {
        fprintf(stderr, "Unhandled argument ignored: `%s'\n", *it);
    }
    
    bool show_help = get_argument_count() == 1 || get_help();

    if( get_verbose() || show_help )
    {
        if( application_text )
        {
            fprintf( stderr, "%s", application_text );
            fprintf( stderr, OPERATING_SYSTEM );
            fprintf( stderr, COMPILER );
            fprintf( stderr, NUMBER_OF_BITS );
            fprintf( stderr, "\n" );
        }
        
        printf("Executable: %s \n", get_application_path() );
        printf("Arguments: ");
        
        for (int i = 1; i < get_argument_count(); i++)
        {
            printf("%s ", get_argument(i));
        }
        
        printf("\n");
    }

    if ( show_help )
    {
        print_help();
        return -1;
    }
    
    if( application_text )
    {
        if( prefix_text )
            fprintf( stderr, "%s %s", prefix_text, application_text );
        else
            fprintf( stderr, "%s", application_text );

        fprintf( stderr, OPERATING_SYSTEM );
        fprintf( stderr, COMPILER );
        fprintf( stderr, NUMBER_OF_BITS );
        fprintf( stderr, "\n" );
    }
    
    return 0;
}

void argument_parser::print_help()
{
    doHelp(cout, command_options);
}

