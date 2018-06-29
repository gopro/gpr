/*! @file gpr_buffer.c
 *
 *  @brief Implementation of gpr_buffer object and functions that work on buffer
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
#include "macros.h"
#include "stdc_includes.h"

int read_from_file(gpr_buffer* buffer, const char* file_path, gpr_malloc malloc_function, gpr_free free_function)
{
    assert( buffer != NULL );
    
    FILE *fIN = fopen(file_path, "rb");
    if (fIN == NULL)
    {
        fprintf (stderr, "Error while reading file: %s", file_path);
        return -1;
    }
    
    fseek (fIN, 0, SEEK_END);
    buffer->size = (size_t) ftell(fIN);
    rewind (fIN);
    
    buffer->buffer = malloc_function(buffer->size);
    
    if ( buffer->buffer == NULL)
    {
        fputs ("Memory error", stderr);
        fclose(fIN);
        return -1;
    }
    
    long result = fread(buffer->buffer, 1, buffer->size, fIN);
    if (result != buffer->size)
    {
        free_function(buffer->buffer);
        fputs ("Reading error", stderr);
        fclose(fIN);
        return -1;
    }
    
    fclose(fIN);
    
    return 0;
}

int write_to_file(const gpr_buffer* buffer, const char* file_path)
{
    unsigned int bytes_written;

    FILE *fOUT = fopen(file_path, "wb");
    if (fOUT == NULL)
    {
        fprintf (stderr, "Error while writing file: %s", file_path);
        return -1;
    }
    
    bytes_written = fwrite(buffer->buffer, 1, buffer->size, fOUT);
    if( bytes_written != buffer->size ) {
        fputs("Could not write bytes \n", stderr);
        perror("fwrite()");
        fclose(fOUT);
        return -2;
    }

    fclose(fOUT);
    
    return 0;
}

#include "gpr_rgb_buffer.h"

void gpr_rgb_gain_set_defaults(gpr_rgb_gain* x)
{
    x->r_gain_num      = 30;
    x->r_gain_pow2_den = 4;
    
    x->g_gain_num      = 1;
    x->g_gain_pow2_den = 0;
    
    x->b_gain_num      = 7;
    x->b_gain_pow2_den = 2;
}

