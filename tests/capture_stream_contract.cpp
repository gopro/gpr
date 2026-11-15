/* Ownership, completion ordering and error isolation on the public row path. */
#include "gpr_capture.h"
#include "gpr_preview_stream.h"
#include "vc5_encoder.h"
#include "vc5_decoder.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <vector>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"capture stream line %d: %s\n",__LINE__,#x);std::abort();}}while(0)
struct Context {
 gpr_capture *capture=nullptr;gpr_preview_stream *stream=nullptr;
 std::vector<uint16_t> input;std::vector<uint8_t> expected,written;
 std::vector<int16_t> planes[3];
 std::atomic<unsigned> released{0},calls{0},observed{0};std::atomic<bool> alive{true};
 std::mutex mutex;std::condition_variable changed;bool block=false,entered=false,unblocked=false,inject=false,fail_write=false,rows_enabled=true;
 Context():input(128*96){
  for(size_t i=0;i<input.size();++i)input[i]=uint16_t((i*173+(i/128)*97)&16383);
  vc5_encoder_parameters p;vc5_encoder_parameters_set_default(&p);p.input_width=128;p.input_height=96;p.input_pitch=256;p.quality_setting=VC5_ENCODER_QUALITY_SETTING_FS1;
  gpr_buffer raw={input.data(),input.size()*2},out={};REQUIRE(!vc5_encoder_process(&p,&raw,&out,nullptr));expected.assign((uint8_t*)out.buffer,(uint8_t*)out.buffer+out.size);
  vc5_decoder_parameters d;vc5_decoder_parameters_set_default(&d);d.mem_alloc=malloc;d.mem_free=free;d.rgb_bits=16;d.rgb_resolution=GPR_RGB_RESOLUTION_QUARTER;
  for(unsigned c=0;c<3;++c){gpr_rgb_buffer plane={};REQUIRE(!vc5_decoder_process_wavelet_channel(&d,&out,c,&plane));planes[c].assign((int16_t*)plane.buffer,(int16_t*)plane.buffer+plane.size/2);free(plane.buffer);}free(out.buffer);
  REQUIRE(!gpr_preview_stream_create(128,96,1u<<20,&stream));
 }
 ~Context(){gpr_preview_stream_destroy(stream);}
 static void release(void*user){auto&s=*static_cast<Context*>(user);s.released.fetch_add(1);std::fill(s.input.begin(),s.input.end(),0);}
 static int write(void*user,const uint8_t*p,size_t n){auto&s=*static_cast<Context*>(user);if(s.fail_write&&n==48&&!memcmp(p,"FRM3",4))return -1;s.written.insert(s.written.end(),p,p+n);return 0;}
 static void row(void*user,uint64_t index,uint64_t timestamp,const vc5_preview_row*r){
  auto&s=*static_cast<Context*>(user);REQUIRE(s.alive&&index==0&&timestamp==900000000000ULL&&r&&r->channel<3);s.calls.fetch_add(1);
  gpr_capture_stats state={};REQUIRE(!gpr_capture_get_stats(s.capture,&state));
  if(s.block&&r->channel==0&&r->level==0&&r->row==0){std::unique_lock<std::mutex>lock(s.mutex);s.entered=true;s.changed.notify_all();s.changed.wait(lock,[&]{return s.unblocked;});}
  if(s.inject&&r->channel==0&&r->level==0&&r->row==0){auto bad=*r;bad.quant[1]=0;gpr_preview_stream_push(s.stream,&bad);}else gpr_preview_stream_push(s.stream,r);
 }
 static void encoded(void*user,const gpr_capture_encoded_frame*f){
  auto&s=*static_cast<Context*>(user);REQUIRE(s.alive&&s.released==1&&f->index==0&&f->timestamp_ns==900000000000ULL);REQUIRE(f->payload_size==s.expected.size()&&!memcmp(f->payload,s.expected.data(),s.expected.size()));
  if(s.rows_enabled){gpr_preview_wavelet_planes out={};int rc=gpr_preview_stream_finish(s.stream,&out);
   if(s.inject)REQUIRE(rc==GPR_PREVIEW_INVALID&&!out.samples[0]);else{REQUIRE(!rc&&out.width==32&&out.height==24);for(unsigned c=0;c<3;++c)REQUIRE(out.sample_count[c]==s.planes[c].size()&&!memcmp(out.samples[c],s.planes[c].data(),s.planes[c].size()*2));}
   REQUIRE(!gpr_preview_stream_reset(s.stream));
  }
  s.observed.fetch_add(1);
 }
};
static void run(bool blocked,bool cancelled,bool inject,bool writer_failure,bool rows=true){
 Context s;s.block=blocked;s.inject=inject;s.fail_write=writer_failure;s.rows_enabled=rows;
 gpr_capture_config config={};config.video={128,96,20,1,3,14,{'R','G','G','B'}};config.stride_bytes=256;config.max_inflight=2;config.workspace_bytes=16u<<20;
 gpr_capture_callbacks cb={Context::write,nullptr,&s};REQUIRE(!gpr_capture_create_with_encoder_rows(&config,&cb,Context::encoded,&s,rows?Context::row:nullptr,&s,&s.capture));
 gpr_capture_frame frame={};frame.data=s.input.data();frame.size_bytes=s.input.size()*2;frame.index=0;frame.timestamp_ns=900000000000ULL;frame.release=Context::release;frame.release_user=&s;
 REQUIRE(!gpr_capture_submit(s.capture,&frame,1000));
 if(blocked){std::unique_lock<std::mutex>lock(s.mutex);REQUIRE(s.changed.wait_for(lock,std::chrono::seconds(10),[&]{return s.entered;}));REQUIRE(!s.released);}
 auto finish=std::async(std::launch::async,[&]{return gpr_capture_finish(s.capture);});
 if(blocked){REQUIRE(finish.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);if(cancelled)gpr_capture_cancel(s.capture);std::lock_guard<std::mutex>lock(s.mutex);s.unblocked=true;s.changed.notify_all();}
 REQUIRE(finish.wait_for(std::chrono::seconds(10))==std::future_status::ready);int error=finish.get();REQUIRE(error==(cancelled?GPR_CAPTURE_CANCELLED:writer_failure?GPR_CAPTURE_WRITER:0));
 REQUIRE(s.released==1&&s.observed==1);if(rows)REQUIRE(s.calls>0);else REQUIRE(!s.calls);
 gpr_capture_stats state={};REQUIRE(!gpr_capture_get_stats(s.capture,&state));if(!cancelled&&!writer_failure)REQUIRE(state.written==1&&!state.error);
 auto calls=s.calls.load();s.alive=false;gpr_capture_destroy(s.capture);s.capture=nullptr;REQUIRE(s.calls==calls&&s.released==1);
}
int main(){run(false,false,false,false);run(true,false,false,false);run(true,true,false,false);run(false,false,true,false);run(false,false,false,true);run(false,false,false,false,false);std::puts("{\"ownership_and_finish\":true,\"cancel_and_writer_error\":true,\"preview_failure_isolated\":true,\"null_rows_ordinary_encode\":true}");}
