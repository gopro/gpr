/*! @file gpr_buffer_auto.cpp
 *
 *  @brief Implementation of gpr_buffer_auto object.
 *
 *  @version 1.0.0
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

#include "gpr_buffer.h"
#include "gpr_buffer_auto.h"

#include <stdlib.h>

int gpr_buffer_auto::read_from_file(const char* file_path)
{
    assert( is_valid() == false );
 
    int return_code = ::read_from_file(&buffer, file_path, mem_alloc, mem_free );

    if( return_code == 0 )
    {
        free_in_destructor = true;
    }
    
    return return_code;
}

int gpr_buffer_auto::write_to_file(const char* file_path)
{
    assert( is_valid() );
    
    return ::write_to_file(&buffer, file_path);
}
