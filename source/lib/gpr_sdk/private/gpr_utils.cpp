
#include "gpr_utils.h"

#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_date_time.h"
#include "dng_exceptions.h"
#include "dng_file_stream.h"
#include "dng_globals.h"
#include "dng_host.h"
#include "dng_ifd.h"
#include "dng_image_writer.h"
#include "dng_info.h"
#include "dng_linearization_info.h"
#include "dng_mosaic_info.h"
#include "dng_negative.h"
#include "dng_preview.h"
#include "dng_render.h"
#include "dng_simple_image.h"
#include "dng_tag_codes.h"
#include "dng_tag_types.h"
#include "dng_tag_values.h"
#include "dng_xmp.h"
#include "dng_xmp_sdk.h"
#include "dng_memory_stream.h"
#include "dng_bottlenecks.h"

#include "gpr_platform.h"
#include "gpr_buffer_auto.h"

void CopyRawImageToBuffer( const dng_image& raw_image, gpr_buffer_auto& buffer )
{
    dng_point size = raw_image.Bounds().Size();
    
    const int raw_image_size = size.h * size.v * 2;
    
    buffer.allocate( raw_image_size );
    
    dng_pixel_buffer pixel_buffer;
    
    pixel_buffer.fArea        = dng_rect(size.v, size.h);
    pixel_buffer.fPlane       = 0;
    pixel_buffer.fPlanes      = 1;
    pixel_buffer.fRowStep     = size.h;
    pixel_buffer.fColStep     = pixel_buffer.fPlanes;
    pixel_buffer.fPlaneStep   = 1;
    pixel_buffer.fPixelType   = ttShort;
    pixel_buffer.fPixelSize   = TagTypeSize(ttShort);
    
    pixel_buffer.fData        = buffer.get_buffer();
    
    raw_image.Get(pixel_buffer);
}

void CopyBufferToRawImage( const gpr_buffer_auto& buffer, size_t stride, dng_image& raw_image )
{
    dng_pixel_buffer pixel_buffer;
    
    dng_point size = raw_image.Bounds().Size();
    
    pixel_buffer.fArea        = dng_rect(size.v, size.h);
    pixel_buffer.fPlane       = 0;
    pixel_buffer.fPlanes      = 1;
    pixel_buffer.fRowStep     = stride;
    pixel_buffer.fColStep     = pixel_buffer.fPlanes;
    pixel_buffer.fPlaneStep   = 1;
    pixel_buffer.fPixelType   = ttShort;
    pixel_buffer.fPixelSize   = TagTypeSize(ttShort);
    
    pixel_buffer.fData        = buffer.get_buffer();
    
    raw_image.Put(pixel_buffer);
}
