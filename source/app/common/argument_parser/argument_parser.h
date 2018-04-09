/*! @file argument_parser.h
 *
 *  @brief Declare class to handle argument parsing
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

#define MAX_ARGC 100

#include "program_options_lite.h"

class argument_parser
{
private:    
    char*   application_path;
    int     argument_count;
    char*   arguments[MAX_ARGC];
    
protected:
    program_options_lite::Options command_options;
    
public:
    argument_parser(bool verbose = true);

    const int   get_argument_count() { return argument_count; }
    const char* get_argument(int index) { return arguments[index]; }
    
    const char* get_application_path() { return application_path; }

    virtual int parse(int argc, char *argv [], const char* application_text = NULL, const char* prefix_text = NULL );
    
    virtual void set_options();
    
    virtual void print_help();
    
    virtual bool get_verbose() { return false; }

    virtual bool get_help() { return false; }
    
};
