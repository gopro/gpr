
#include "gpr_image_writer.h"

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

#if GPR_WRITING
#include "vc5_encoder.h"
#endif

#include "gpr_utils.h"

#if GPR_WRITING

gpr_image_writer::gpr_image_writer(const gpr_buffer_auto* raw_buffer,
                                           unsigned int raw_buffer_width,
                                           unsigned int raw_buffer_height,
                                           size_t       raw_buffer_pitch,
                                           gpr_buffer_auto* vc5_buffer
                                           ) :   _vc5_buffer_obj( raw_buffer->get_malloc(), raw_buffer->get_free() ),
                                                _vc5_buffer(vc5_buffer),
                                                _raw_buffer(raw_buffer)
{
    if( _vc5_buffer == NULL )
    {
        _vc5_buffer = &_vc5_buffer_obj;
    }
    
    _rgb_thumbnail.buffer = NULL;
    _rgb_thumbnail.size = 0;
    
    vc5_encoder_parameters_set_default(&vc5_encoder_params);
}

gpr_image_writer::~gpr_image_writer()
{
    if( _rgb_thumbnail.buffer )
    {
        assert( _rgb_thumbnail.size > 0 );
        
        _raw_buffer->get_free()(_rgb_thumbnail.buffer);
        _rgb_thumbnail.buffer = NULL;
        _rgb_thumbnail.size = 0;
    }
}


void gpr_image_writer::EncodeVc5Image()
{
    if( _vc5_buffer->is_valid() == false )
    {
        
        gpr_buffer raw_image = { _raw_buffer->get_buffer(), _raw_buffer->get_size() };
        gpr_buffer vc5_image = { _vc5_buffer->get_buffer(), _vc5_buffer->get_size() };
        
        if( vc5_encoder_process( &vc5_encoder_params, &raw_image, &vc5_image, &_rgb_thumbnail ) != CODEC_ERROR_OKAY )
        {
            assert(0);
        }
        
        _vc5_buffer->set( vc5_image.buffer, vc5_image.size, true );
    }
}

void gpr_image_writer::WriteImage (dng_host &host,
                                       const dng_ifd &ifd,
                                       dng_basic_tag_set &basic,
                                       dng_stream &stream,
                                       const dng_image &image,
                                       uint32 fakeChannels)
{
    dng_image_writer::WriteImage ( host, ifd, basic, stream, image, fakeChannels);
}

uint32 gpr_image_writer::CompressedBufferSize (const dng_ifd &ifd, uint32 uncompressedSize)
{
    if( ifd.fCompression == ccVc5 )
    {
        return _vc5_buffer->get_size();
    }
    else
    {
        return dng_image_writer::CompressedBufferSize( ifd, uncompressedSize );
    }
}

void gpr_image_writer::WriteTile (dng_host &host,
                                      const dng_ifd &ifd,
                                      dng_stream &stream,
                                      const dng_image &image,
                                      const dng_rect &tileArea,
                                      uint32 fakeChannels,
                                      AutoPtr<dng_memory_block> &compressedBuffer,
                                      AutoPtr<dng_memory_block> &uncompressedBuffer,
                                      AutoPtr<dng_memory_block> &subTileBlockBuffer,
                                      AutoPtr<dng_memory_block> &tempBuffer)
{
    if( ifd.fCompression == ccVc5 )
    {
        stream.Put( _vc5_buffer->get_buffer(), _vc5_buffer->get_size() );
    }
    else
    {
        return dng_image_writer::WriteTile( host, ifd, stream, image, tileArea, fakeChannels, compressedBuffer, uncompressedBuffer, subTileBlockBuffer, tempBuffer );
    }
}

#endif // GPR_WRITING
