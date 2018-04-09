
#include "gpr_read_image.h"

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

#include "gpr_utils.h"

#if GPR_READING

#include "vc5_decoder.h"

static bool DecodeVC5(dng_image &image, gpr_buffer_auto& vc5_buffer, VC5_DECODER_PIXEL_FORMAT pixel_format )
{
    gpr_buffer_auto raw_buffer( malloc, free );
    
    vc5_decoder_parameters vc5_decoder_params;
    
    vc5_decoder_parameters_set_default(&vc5_decoder_params);
    
    vc5_decoder_params.mem_alloc        = raw_buffer.get_malloc();
    vc5_decoder_params.mem_free         = raw_buffer.get_free();
    vc5_decoder_params.pixel_format     = pixel_format;
    
    gpr_buffer vc5_image = { vc5_buffer.get_buffer(), vc5_buffer.get_size()  };
    gpr_buffer raw_image = { raw_buffer.get_buffer(), raw_buffer.get_size()  };
    
    if( vc5_decoder_process( &vc5_decoder_params, &vc5_image, &raw_image, NULL ) != CODEC_ERROR_OKAY )
    {
        assert(0);
        return false;
    }
    
    raw_buffer.set( raw_image.buffer, raw_image.size, true );
    
    dng_point size = image.Bounds().Size();
    
    CopyBufferToRawImage( raw_buffer, size.h, image );

    return true;
}

gpr_read_image::gpr_read_image( gpr_buffer_auto* vc5_buffer ) : _vc5_buffer(vc5_buffer)
{
    fReadVC5 = true;
    fDecodeVC5 = true;
}

void gpr_read_image::ReadTile (dng_host &host,
                                   const dng_ifd &ifd,
                                   dng_stream &stream,
                                   dng_image &image,
                                   const dng_rect &tileArea,
                                   uint32 plane,
                                   uint32 planes,
                                   uint32 tileByteCount,
                                   AutoPtr<dng_memory_block> &compressedBuffer,
                                   AutoPtr<dng_memory_block> &uncompressedBuffer,
                                   AutoPtr<dng_memory_block> &subTileBlockBuffer)
{
    
    if( ifd.fCompression == ccVc5 )
    {
        if( GetReadVC5() && _vc5_buffer != NULL )
        {
            _vc5_buffer->allocate( tileByteCount );
            
            stream.Get(_vc5_buffer->get_buffer(), _vc5_buffer->get_size() );
            
            if( GetDecodeVC5() == true )
            {
                bool rggb_raw = (ifd.fCFAPattern[0][0] == 0) && (ifd.fCFAPattern[0][1] == 1) && (ifd.fCFAPattern[1][0] == 1) && (ifd.fCFAPattern[1][1] == 2);
                
                VC5_DECODER_PIXEL_FORMAT pixel_format;
                
                if( rggb_raw )
                {
                    pixel_format = VC5_DECODER_PIXEL_FORMAT_RGGB_14;
                }
                else
                {
                    pixel_format = VC5_DECODER_PIXEL_FORMAT_GBRG_12;
                }
                
                if( DecodeVC5( image, *_vc5_buffer, pixel_format ) )
                {
                    return;
                }
            }
        }
    }
    else
    {
        dng_read_image::ReadTile (host,
                                  ifd,
                                  stream,
                                  image,
                                  tileArea,
                                  plane,
                                  planes,
                                  tileByteCount,
                                  compressedBuffer,
                                  uncompressedBuffer,
                                  subTileBlockBuffer);
    }
}

#endif
