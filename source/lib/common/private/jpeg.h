/*! @file jpeg.h
 *
 *  @brief JPEG (JPG) encoding helpers for the GPR SDK.
 *
 *  Encodes interleaved 8-bit RGB into an in-memory JPEG, embedding an EXIF
 *  Orientation tag so that viewers display the image the right way up without
 *  the pixels being physically rotated. Wraps the tiny_jpeg encoder.
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

#ifndef GPR_JPEG_H
#define GPR_JPEG_H

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "gpr_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Read the pixel dimensions of a JPEG image from its header, without decoding it.
// Walks the marker segments until the Start-Of-Frame (SOF) marker, which carries the
// height and width. Returns 1 and fills *width/*height on success, 0 otherwise.
static int gpr_jpeg_get_dimensions( const unsigned char* d, size_t n, int* width, int* height )
{
    size_t i;

    if( d == NULL || n < 4 || d[0] != 0xFF || d[1] != 0xD8 ) // SOI
        return 0;

    i = 2;
    while( i + 1 < n )
    {
        unsigned char marker;
        int seg_len;
        bool is_sof;

        if( d[i] != 0xFF ) { i++; continue; }    // skip fill bytes until a marker
        marker = d[i + 1];
        i += 2;

        // Standalone markers carry no length: SOI, EOI, TEM and the restart markers.
        if( marker == 0xD8 || marker == 0xD9 || marker == 0x01 || ( marker >= 0xD0 && marker <= 0xD7 ) )
            continue;

        if( i + 2 > n )
            break;

        seg_len = ( d[i] << 8 ) | d[i + 1];      // length includes these 2 bytes
        if( seg_len < 2 )
            break;

        // SOF markers (0xC0-0xCF) carry the frame dimensions, except DHT(C4), JPG(C8), DAC(CC).
        is_sof = ( marker >= 0xC0 && marker <= 0xCF ) && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if( is_sof )
        {
            if( i + 7 > n )                      // length(2) precision(1) height(2) width(2)
                break;

            *height = ( d[i + 3] << 8 ) | d[i + 4];
            *width  = ( d[i + 5] << 8 ) | d[i + 6];
            return ( *width > 0 && *height > 0 ) ? 1 : 0;
        }

        i += seg_len;                            // skip this segment
    }

    return 0;
}

// The remaining helpers encode JPEGs and depend on the tiny_jpeg encoder, so they are only
// available when JPEG writing is compiled in (GPR_JPEG_AVAILABLE).
#if GPR_JPEG_AVAILABLE

#include "tiny_jpeg.h"

// Convert a DNG/Adobe orientation (0-7) to its EXIF/TIFF Orientation value (1-8).
static int gpr_adobe_orientation_to_exif( int adobe_orientation )
{
    // Indexed by the Adobe orientation enum (kNormal=0 .. kMirror90CCW=7).
    static const int exif[8] = { 1, 6, 3, 8, 2, 7, 4, 5 };
    return ( adobe_orientation >= 0 && adobe_orientation < 8 ) ? exif[adobe_orientation] : 1;
}

// Growable byte buffer used to collect the JPEG produced by tje_encode_with_func.
typedef struct
{
    unsigned char*  data;
    size_t          size;
    size_t          capacity;

} gpr_jpg_sink;

static void gpr_jpg_sink_write( void* context, void* data, int size )
{
    gpr_jpg_sink* sink = (gpr_jpg_sink*)context;

    if( sink->size + (size_t)size > sink->capacity )
    {
        size_t new_capacity = sink->capacity ? sink->capacity : ( 1 << 20 );
        while( new_capacity < sink->size + (size_t)size )
            new_capacity *= 2;

        sink->data = (unsigned char*)realloc( sink->data, new_capacity );
        sink->capacity = new_capacity;
    }

    if( sink->data )
    {
        memcpy( sink->data + sink->size, data, (size_t)size );
        sink->size += (size_t)size;
    }
}

// Encode interleaved 8-bit RGB into an in-memory JPEG with an embedded EXIF Orientation tag.
// exif_orientation is a TIFF/EXIF Orientation value (1-8). out_jpg is allocated with
// allocator->Alloc (free it with the matching allocator->Free). Returns true on success.
static bool gpr_encode_rgb_to_jpg( const gpr_allocator* allocator,
                                   const unsigned char* rgb, int width, int height,
                                   int quality, int exif_orientation,
                                   gpr_buffer* out_jpg )
{
    // Minimal EXIF APP1 segment (little-endian TIFF) with two IFD0 tags in ascending order:
    // Orientation (0x0112) and YCbCrPositioning (0x0213, required for a conformant JPEG EXIF).
    unsigned char app1[] =
    {
        0xFF, 0xE1, 0x00, 0x2E,                          // APP1 marker, segment length = 46
        'E',  'x',  'i',  'f',  0x00, 0x00,              // "Exif\0\0"
        'I',  'I',  0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,  // TIFF header (LE), IFD0 at offset 8
        0x02, 0x00,                                      // IFD0: 2 entries
        0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,  // tag 0x0112 Orientation, SHORT, count 1
        0x00, 0x00, 0x00, 0x00,                          // Orientation value (set below) + padding
        0x13, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,  // tag 0x0213 YCbCrPositioning, SHORT, count 1
        0x01, 0x00, 0x00, 0x00,                          // YCbCrPositioning value = 1 (centered) + padding
        0x00, 0x00, 0x00, 0x00                           // next IFD offset = 0
    };

    gpr_jpg_sink sink = { NULL, 0, 0 };
    unsigned char* out;

    // tinyjpeg only supports quality levels 1 (lowest), 2, or 3 (highest).
    if( quality < 1 ) quality = 1;
    if( quality > 3 ) quality = 3;

    app1[28] = (unsigned char)( exif_orientation & 0xFF ); // Orientation SHORT (low byte)
    app1[29] = 0x00;

    if( tje_encode_with_func( gpr_jpg_sink_write, &sink, quality, width, height, 3, rgb ) == 0 || sink.data == NULL )
    {
        free( sink.data );
        return false;
    }

    // Insert the EXIF segment right after the SOI marker (FFD8) so it is the first segment.
    if( sink.size >= 2 && sink.data[0] == 0xFF && sink.data[1] == 0xD8 )
    {
        out_jpg->size   = sink.size + sizeof(app1);
        out_jpg->buffer = allocator->Alloc( out_jpg->size );

        if( out_jpg->buffer == NULL ) { free( sink.data ); return false; }

        out = (unsigned char*)out_jpg->buffer;
        memcpy( out,                     sink.data,     2 );               // SOI
        memcpy( out + 2,                 app1,          sizeof(app1) );    // EXIF APP1
        memcpy( out + 2 + sizeof(app1),  sink.data + 2, sink.size - 2 );   // rest of the JPEG
    }
    else
    {
        out_jpg->size   = sink.size;
        out_jpg->buffer = allocator->Alloc( out_jpg->size );

        if( out_jpg->buffer == NULL ) { free( sink.data ); return false; }

        memcpy( out_jpg->buffer, sink.data, sink.size );
    }

    free( sink.data );

    return true;
}

#endif // GPR_JPEG_AVAILABLE

#ifdef __cplusplus
}
#endif

#endif // GPR_JPEG_H
