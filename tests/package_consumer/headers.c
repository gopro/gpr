/* Compile public headers as C without private headers or prerequisite includes. */
#if HAS_ENCODER
#include "vc5_encoder.h"
#endif
#if HAS_DECODER
#include "vc5_decoder.h"
static int c_preview_boundary(void) {
    gpr_rgb_buffer planes[3]={{0}};
    return vc5_decoder_reconstruct_preview(0,0,0,planes)==CODEC_ERROR_BAD_ARGUMENT;
}
#endif
#if HAS_SDK && !NATIVE_CLIENT
#include "gpr.h"
#endif
#if HAS_CAPTURE
#include "gpr_capture.h"
#include "gpr_gvid_reader.h"
#endif
#if HAS_PREVIEW
#include "gpr_preview.h"
#include "gpr_preview_queue.h"
#include "gpr_preview_stream.h"
static int c_display(void *user,const gpr_preview_frame_view *frame) { (void)user;(void)frame;return 0; }
#endif
int c_header_probe(void) {
#if HAS_CAPTURE
    if (gpr_capture_create_with_encoder_rows(0,0,0,0,0,0,0)!=GPR_CAPTURE_INVALID) return 0;
    if (gpr_capture_set_encoder_wait(0,0,0)!=GPR_CAPTURE_INVALID) return 0;
#endif
#if HAS_DECODER
    if (!c_preview_boundary()) return 0;
#endif
#if HAS_PREVIEW
    gpr_gvid_config video={128,96,30,1,3,14,{'R','G','G','B'}};
    gpr_preview_stream *stream=0;gpr_preview_wavelet_planes planes={0};
    if (gpr_preview_stream_create(128,96,1024*1024,&stream)) return 0;
    if (!gpr_preview_stream_buffer_bytes(stream) || gpr_preview_stream_finish(stream,&planes)!=GPR_PREVIEW_PAYLOAD) {gpr_preview_stream_destroy(stream);return 0;}
    if (gpr_preview_stream_reset(stream)) {gpr_preview_stream_destroy(stream);return 0;}
    gpr_preview_stream_destroy(stream);
    gpr_preview_calibration calibration;
    gpr_preview_plan *plan=0;
    gpr_preview_info info;
    gpr_preview_calibration_identity(&video,&calibration);
    if (gpr_preview_validate_payload(0,0,0)!=GPR_PREVIEW_INVALID) return 0;
    if (gpr_preview_queue_offer_wavelets(0,0,0,0,0,0)!=GPR_PREVIEW_INVALID) return 0;
    if (gpr_preview_plan_create(&video,&calibration,&plan)) return 0;
    if (gpr_preview_plan_info(plan,&info) || info.width!=32 || info.height!=24) {
        gpr_preview_plan_destroy(plan);return 0;
    }
    if (gpr_preview_plan_render_wavelets(plan,0,0,0,0,&info)!=GPR_PREVIEW_INVALID) {
        gpr_preview_plan_destroy(plan);return 0;
    }
    gpr_preview_plan_destroy(plan);
    {
        gpr_preview_queue_config config={1,1,c_display,0};gpr_preview_queue *queue=0;
        if (gpr_preview_queue_create(&config,&queue)) return 0;
        if (gpr_preview_queue_flush(queue)) {gpr_preview_queue_destroy(queue);return 0;}
        if (gpr_preview_queue_finish(queue)) {gpr_preview_queue_destroy(queue);return 0;}
        gpr_preview_queue_destroy(queue);
    }
#endif
#if HAS_SDK && !NATIVE_CLIENT
    gpr_parameters p,copy;
    gpr_allocator allocator={malloc,free};
    gpr_buffer empty={0};
    gpr_parameters_set_defaults(&p);
    p.input_width=128;
    gpr_parameters_construct_copy(&p,&copy);
    if (copy.input_width!=128) return 0;
    gpr_parameters_destroy(&copy,free);
    if (!gpr_parameters_construct_copy_with_allocator(&allocator,&p,&copy)) return 0;
    gpr_parameters_destroy(&copy,free);
    if (gpr_check_vc5(&empty,malloc,free)) return 0;
    gpr_parameters_destroy(&p,free);
    return 1;
#elif HAS_ENCODER
    vc5_encoder_parameters p;
    vc5_encoder_parameters_set_default(&p);
    if (vc5_encoder_process_with_preview_rows(0,0,0,0,0,0)!=CODEC_ERROR_BAD_ARGUMENT) return 0;
    if (vc5_encoder_process_with_preview(0,0,0,0,0,0)!=CODEC_ERROR_BAD_ARGUMENT) return 0;
    return p.input_width > 0;
#elif HAS_DECODER
    vc5_decoder_parameters p;
    vc5_decoder_parameters_set_default(&p);
    return p.rgb_bits == 8 || p.rgb_bits == 16;
#else
    return 1;
#endif
}
