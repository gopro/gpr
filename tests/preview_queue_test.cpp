/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_preview_queue.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
static void check(bool value,const char *why) { if (!value) throw std::runtime_error(why); }
using Plan=std::unique_ptr<gpr_preview_plan,decltype(&gpr_preview_plan_destroy)>;
using Queue=std::unique_ptr<gpr_preview_queue,decltype(&gpr_preview_queue_destroy)>;
struct Sink {
    gpr_preview_queue *queue=nullptr;
    std::mutex mutex;std::condition_variable changed;
    bool block=false,entered=false,allow=false,fail=false;
    std::vector<uint64_t> indices;
    std::vector<uint32_t> payloads,pixels;
    static int display(void *user,const gpr_preview_frame_view *frame) {
        auto &s=*static_cast<Sink *>(user);gpr_preview_queue_stats stats={};
        if (gpr_preview_queue_get_stats(s.queue,&stats)) return 1; // callback is outside queue locks
        std::unique_lock<std::mutex> lock(s.mutex);
        s.indices.push_back(frame->index);s.payloads.push_back(frame->payload_crc32);
        s.pixels.push_back(gpr_gvid_crc32(0,frame->rgb,frame->size_bytes));
        s.entered=true;s.changed.notify_all();
        if (s.block) s.changed.wait(lock,[&]{return s.allow;});
        return s.fail ? 1:0;
    }
    void wait() { std::unique_lock<std::mutex> lock(mutex);check(changed.wait_for(lock,std::chrono::seconds(5),[&]{return entered;}),"sink entry timeout"); }
    void unblock() { std::lock_guard<std::mutex> lock(mutex);allow=true;changed.notify_all(); }
};
static Plan plan() {
    gpr_gvid_config v={128,96,30,1,3,14,{'R','G','G','B'}};gpr_preview_calibration c={};
    gpr_preview_calibration_identity(&v,&c);gpr_preview_plan *created=nullptr;
    check(!gpr_preview_plan_create(&v,&c,&created),"plan create");return Plan(created,gpr_preview_plan_destroy);
}
static Queue queue(Sink &sink,size_t max_payload,size_t rgb=32*24*3) {
    gpr_preview_queue_config c={max_payload,rgb,Sink::display,&sink};gpr_preview_queue *created=nullptr;
    check(!gpr_preview_queue_create(&c,&created),"queue create");sink.queue=created;return Queue(created,gpr_preview_queue_destroy);
}
static void latest(const std::vector<uint8_t> &payload) {
    auto p=plan();Sink sink;sink.block=true;auto q=queue(sink,payload.size());
    std::vector<uint8_t> expected(32*24*3),input=payload;gpr_preview_info info={};
    check(!gpr_preview_plan_render(p.get(),payload.data(),payload.size(),expected.data(),expected.size(),96,&info),"direct render");
    check(!gpr_preview_queue_offer(q.get(),p.get(),0,1000,input.data(),input.size()),"first offer");sink.wait();
    for (unsigned i=1;i<8;++i) {
        input=payload;check(!gpr_preview_queue_offer(q.get(),p.get(),i,1000+i,input.data(),input.size()),"latest offer");
        std::fill(input.begin(),input.end(),0); // borrowed source expires immediately
    }
    gpr_preview_queue_stats stats={};gpr_preview_queue_get_stats(q.get(),&stats);
    check(stats.offered==8 && stats.inflight==2 && stats.inflight_high_water==2 && stats.replaced==6 && stats.discarded==6,"bounded latest pending");
    check(stats.payload_peak_bytes==payload.size()*2,"bounded payload accounting");
    int result[2]={-99,-99};std::thread finishers[2];
    for (unsigned i=0;i<2;++i) finishers[i]=std::thread([&,i]{result[i]=gpr_preview_queue_finish(q.get());});
    // Wait until at least one finisher has closed submission without relying on a sleep.
    int closed=0;uint64_t next=8;
    do { closed=gpr_preview_queue_offer(q.get(),p.get(),next,1000+next,payload.data(),payload.size());++next;std::this_thread::yield(); } while (closed==0);
    check(closed==GPR_PREVIEW_CLOSED,"finish closes submission");
    p.reset(); // queue owns all remaining plan references
    sink.unblock();for (auto &thread:finishers) thread.join();
    check(!result[0] && !result[1],"concurrent finish");
    gpr_preview_queue_get_stats(q.get(),&stats);
    check(stats.inflight==0 && stats.rendered==2 && stats.offered==stats.rendered+stats.discarded,"complete queue accounting");
    check(sink.indices.size()==2 && sink.indices.front()==0 && sink.indices.back()>=7,"newest frame drain");
    for (uint32_t crc:sink.payloads) check(crc==gpr_gvid_crc32(0,payload.data(),payload.size()),"queue retained borrowed payload");
    for (uint32_t crc:sink.pixels) check(crc==gpr_gvid_crc32(0,expected.data(),expected.size()),"queued pixels differ");
}
static void cancellation(const std::vector<uint8_t> &payload) {
    auto p=plan();Sink sink;sink.block=true;auto q=queue(sink,payload.size());
    check(!gpr_preview_queue_offer(q.get(),p.get(),0,100,payload.data(),payload.size()),"cancel first");sink.wait();
    check(!gpr_preview_queue_offer(q.get(),p.get(),1,101,payload.data(),payload.size()),"cancel pending");
    gpr_preview_queue_cancel(q.get());p.reset();sink.unblock();check(!gpr_preview_queue_finish(q.get()),"cancel drain");
    gpr_preview_queue_stats stats={};gpr_preview_queue_get_stats(q.get(),&stats);
    check(stats.offered==2 && stats.rendered==1 && stats.discarded==1 && !stats.inflight,"cancel discards pending only");
}
static void failures(const std::vector<uint8_t> &payload) {
    auto p=plan();Sink sink;sink.fail=true;auto q=queue(sink,payload.size());
    check(!gpr_preview_queue_offer(q.get(),p.get(),0,100,payload.data(),payload.size()),"failing callback offer");
    check(gpr_preview_queue_flush(q.get())==GPR_PREVIEW_CALLBACK,"flush propagates sink failure");
    check(gpr_preview_queue_finish(q.get())==GPR_PREVIEW_CALLBACK,"sink failure latches");
    check(gpr_preview_queue_offer(q.get(),p.get(),1,101,payload.data(),payload.size())==GPR_PREVIEW_CALLBACK,"failure sticky");
    Sink bad_sink;auto bad=queue(bad_sink,payload.size());std::vector<uint8_t> corrupt(64,0);
    check(!gpr_preview_queue_offer(bad.get(),p.get(),0,100,corrupt.data(),corrupt.size()),"bad codec offer");
    check(gpr_preview_queue_finish(bad.get())==GPR_PREVIEW_PAYLOAD && bad_sink.indices.empty(),"bad codec does not deliver pixels");
    Sink small_sink;auto small=queue(small_sink,payload.size(),1);
    check(gpr_preview_queue_offer(small.get(),p.get(),0,100,payload.data(),payload.size())==GPR_PREVIEW_CAPACITY,"RGB capacity enforced before ownership");
    check(!gpr_preview_queue_finish(small.get()),"empty finish");
    gpr_preview_queue_stats stats={};gpr_preview_queue_get_stats(small.get(),&stats);check(!stats.offered && !stats.inflight,"rejected offer ownership");
}
static void reusable_flush(const std::vector<uint8_t> &payload) {
    check(gpr_preview_queue_flush(nullptr)==GPR_PREVIEW_INVALID,"null flush");
    auto p=plan();Sink sink;sink.block=true;auto q=queue(sink,payload.size());
    check(!gpr_preview_queue_flush(q.get()),"empty flush");
    check(!gpr_preview_queue_offer(q.get(),p.get(),0,100,payload.data(),payload.size()),"flush active offer");sink.wait();
    check(!gpr_preview_queue_offer(q.get(),p.get(),1,101,payload.data(),payload.size()),"flush pending offer");
    auto flushing=std::async(std::launch::async,[&]{return gpr_preview_queue_flush(q.get());});
    bool waited=flushing.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout;
    sink.unblock();int result=flushing.get();
    check(waited && !result,"flush did not wait for active callback");
    gpr_preview_queue_stats stats={};gpr_preview_queue_get_stats(q.get(),&stats);
    check(!stats.inflight && stats.rendered==2 && !stats.discarded,"flush returned before queue drained");
    check(!gpr_preview_queue_offer(q.get(),p.get(),2,102,payload.data(),payload.size()),"flush closed submission");
    check(!gpr_preview_queue_flush(q.get()),"second flush");
    check(!gpr_preview_queue_finish(q.get()) && !gpr_preview_queue_flush(q.get()),"flush after finish");
    check(sink.indices==std::vector<uint64_t>({0,1,2}),"flush lost/reordered delivery");
}
int main(int argc,char **argv) {
    try {
        check(argc==2,"fixture required");std::ifstream in(argv[1],std::ios::binary|std::ios::ate);check(bool(in),"fixture open");
        std::vector<uint8_t> payload(size_t(in.tellg()));in.seekg(0);check(bool(in.read(reinterpret_cast<char *>(payload.data()),payload.size())),"fixture read");
        latest(payload);cancellation(payload);failures(payload);reusable_flush(payload);
        std::cout<<"PASS: bounded latest-frame queue, copied payloads, retained plans, callback/stats, concurrent finish, cancellation and failures\n";
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n';return 1; }
}
