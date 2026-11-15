/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_capture.h"
#include "vc5_encoder.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <future>

static void check(bool okay,const char *why) { if (!okay) throw std::runtime_error(why); }
static uint64_t get(const uint8_t *p,unsigned n) {
    uint64_t v=0; for (unsigned i=0;i<n;++i) v|=uint64_t(p[i])<<(i*8); return v;
}
struct Sink {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> observed;
    bool block_observer=false,observer_entered=false,observer_released=false;
    gpr_capture *capture=nullptr;
    static void encoded(void *user,const gpr_capture_encoded_frame *frame) {
        Sink &s=*static_cast<Sink *>(user);
        gpr_capture_stats state={};
        if (s.capture) check(!gpr_capture_get_stats(s.capture,&state),"observer stats reentry");
        std::unique_lock<std::mutex> lock(s.mutex);
        check(frame->index==s.observed.size(),"observer frame order");
        check(frame->timestamp_ns==900000000000ULL+frame->index*33410000,"observer timestamp");
        check(frame->payload && frame->payload_size>=64 && !std::memcmp(frame->payload,"VC-5",4),"observer payload");
        s.observed.emplace_back(frame->payload,frame->payload+frame->payload_size);
        if (s.block_observer) {
            s.observer_entered=true;s.changed.notify_all();
            s.changed.wait(lock,[&]{return s.observer_released;});
            check(!std::memcmp(s.observed.back().data(),frame->payload,frame->payload_size),"borrowed observer payload expired");
        }
    }
    bool block=false, entered=false, released=false, fail=false, fail_end=false;
    unsigned completions=0;
    static int write(void *user,const uint8_t *p,size_t n) {
        Sink &s=*static_cast<Sink *>(user);
        std::unique_lock<std::mutex> lock(s.mutex);
        bool frame=n==48 && !std::memcmp(p,"FRM3",4);
        if (frame) check(get(p+8,8)<s.observed.size(),"storage started before observer returned");
        if (frame && s.block && !s.entered) {
            s.entered=true; s.changed.notify_all();
            s.changed.wait(lock,[&]{ return s.released; });
        }
        if ((frame && s.fail) || (s.fail_end && n==48 && !std::memcmp(p,"END3",4))) return -1;
        s.bytes.insert(s.bytes.end(),p,p+n); return 0;
    }
    static void complete(void *user,const gpr_capture_completion *) {
        ++static_cast<Sink *>(user)->completions;
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock,std::chrono::seconds(10),[&]{ return entered; }),"writer gate timeout");
    }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); released=true; changed.notify_all(); }
};
struct Capture {
    gpr_capture *p=nullptr;
    Capture(const gpr_capture_config &cfg,Sink &sink) {
        gpr_capture_callbacks callbacks={Sink::write,Sink::complete,&sink};
        check(!gpr_capture_create_with_observer(&cfg,&callbacks,Sink::encoded,&sink,&p),"create failed");
        sink.capture=p;
    }
    ~Capture() { if (p) gpr_capture_destroy(p); }
};
static gpr_capture_config config(unsigned count=2,size_t workspace=0) {
    gpr_capture_config c={}; c.video={128,96,30000,1001,3,14,{'R','G','G','B'}};
    c.stride_bytes=128*2+16; c.max_inflight=count; c.workspace_bytes=workspace;
    c.metadata="{\"camera\":\"test\",\"black_level\":0,\"white_level\":16383}";
    c.metadata_size=std::strlen(static_cast<const char *>(c.metadata)); return c;
}
static std::vector<uint8_t> image(unsigned seed) {
    std::vector<uint8_t> data((128*2+16)*96,0x55);
    for (unsigned y=0;y<96;++y) for (unsigned x=0;x<128;++x) {
        uint16_t v=uint16_t((x*171+y*93+seed*871)&16383);
        std::memcpy(data.data()+y*(128*2+16)+x*2,&v,2);
    }
    return data;
}
static gpr_capture_frame frame(std::vector<uint8_t> &data,uint64_t index) {
    gpr_capture_frame f={data.data(),data.size(),index,900000000000ULL+index*33410000,
                         "{\"exposure_us\":1000}",20}; return f;
}
static std::vector<uint8_t> reference(std::vector<uint8_t> source,
                    VC5_ENCODER_PIXEL_FORMAT format=VC5_ENCODER_PIXEL_FORMAT_RGGB_14) {
    std::vector<uint8_t> packed(128*96*2);
    for (unsigned y=0;y<96;++y) std::memcpy(packed.data()+y*256,source.data()+y*272,256);
    vc5_encoder_parameters p={}; vc5_encoder_parameters_set_default(&p);
    p.input_width=128; p.input_height=96; p.input_pitch=256; p.quality_setting=VC5_ENCODER_QUALITY_SETTING_FS1;
    p.pixel_format=format;
    gpr_buffer input={packed.data(),packed.size()}, output={nullptr,0};
    check(!vc5_encoder_process(&p,&input,&output,nullptr),"reference encode");
    std::vector<uint8_t> result(static_cast<uint8_t *>(output.buffer),static_cast<uint8_t *>(output.buffer)+output.size);
    free(output.buffer); return result;
}
static void validate(const Sink &sink,const std::vector<std::vector<uint8_t>> &expected) {
    const auto &data=sink.bytes;
    check(data.size()>=64+48,"short capture");
    check(!std::memcmp(data.data(),"GVID\3\0",6),"wrong profile");
    size_t offset=64+get(data.data()+44,4);
    uint64_t payload_total=0;
    for (size_t i=0;i<expected.size();++i) {
        check(offset+48<=data.size(),"short frame header");
        const uint8_t *p=data.data()+offset;
        check(!std::memcmp(p,"FRM3",4) && get(p+8,8)==i,"frame sequence");
        check(get(p+16,8)==900000000000ULL+i*33410000,"timestamp lost");
        size_t n=size_t(get(p+24,8)), metadata=size_t(get(p+32,4));
        check(metadata==20,"frame metadata lost");
        check(!std::memcmp(p+48,"{\"exposure_us\":1000}",20),"frame metadata not copied");
        check(offset+48+metadata+n<=data.size(),"short payload");
        check(n==expected[i].size() && !std::memcmp(p+48+metadata,expected[i].data(),n),"source ownership/stock bytes changed");
        check(i<sink.observed.size() && sink.observed[i]==expected[i],"observer differs from recorded/reference payload");
        check(get(p+36,4)==gpr_gvid_crc32(0,p+48+metadata,n),"payload crc");
        payload_total+=n; offset+=48+metadata+n;
    }
    check(offset+48==data.size(),"missing end or trailing bytes");
    const uint8_t *end=data.data()+offset;
    check(!std::memcmp(end,"END3",4) && get(end+8,8)==expected.size(),"end count");
    check(get(end+24,8)==payload_total,"end payload bytes");
    check(get(end+32,4)==gpr_gvid_crc32(0,data.data(),offset),"stream crc");
    check(get(end+36,4)==gpr_gvid_crc32(0,end,36),"end crc");
}
static void ownership(const char *path) {
    Sink sink; sink.block=true;
    auto cfg=config(2,1); std::string stream_metadata(static_cast<const char *>(cfg.metadata),cfg.metadata_size);
    cfg.metadata=stream_metadata.data(); Capture c(cfg,sink);
    stream_metadata.assign(stream_metadata.size(),'x');
    auto a=image(1),b=image(2),a_original=a,b_original=b;
    std::string metadata="{\"exposure_us\":1000}";
    auto f=frame(a,0); f.metadata=metadata.data();
    check(!gpr_capture_submit(c.p,&f,0),"submit first"); metadata.assign(metadata.size(),'x');
    std::fill(a.begin(),a.end(),0); sink.wait();
    f=frame(b,1); check(!gpr_capture_submit(c.p,&f,0),"submit second");
    std::fill(b.begin(),b.end(),0);
    f=frame(a,2); check(gpr_capture_submit(c.p,&f,0)==GPR_CAPTURE_BUSY,"unbounded queue");
    check(gpr_capture_submit(c.p,&f,2)==GPR_CAPTURE_BUSY,"bounded wait ignored");
    sink.unblock(); check(!gpr_capture_finish(c.p),"finish");
    gpr_capture_stats stats={}; check(!gpr_capture_get_stats(c.p,&stats),"stats");
    check(stats.submitted==2 && stats.written==2 && stats.inflight==0 && stats.inflight_high_water==2,"accounting");
    check(stats.workspace_fallback_peak_bytes>0,"fallback not exercised");
    check(sink.completions==2,"completion count");
    check(gpr_capture_submit(c.p,&f,0)==GPR_CAPTURE_CLOSED,"finish accepts frames");
    validate(sink,{reference(a_original),reference(b_original)});
    if (path) { std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char *>(sink.bytes.data()),sink.bytes.size()); check(bool(out),"fixture output"); }
}
static void lifecycle() {
    Sink sink; Capture c(config(),sink); auto data=image(3); auto f=frame(data,0);
    auto invalid=f; invalid.index=1;
    check(gpr_capture_submit(c.p,&invalid,0)==GPR_CAPTURE_INVALID,"bad index");
    invalid=f; invalid.size_bytes=4;
    check(gpr_capture_submit(c.p,&invalid,0)==GPR_CAPTURE_INVALID,"short raw");
    check(!gpr_capture_submit(c.p,&f,UINT32_MAX) && !gpr_capture_flush(c.p),"flush");
    invalid=frame(data,1); invalid.timestamp_ns=f.timestamp_ns;
    check(gpr_capture_submit(c.p,&invalid,0)==GPR_CAPTURE_INVALID,"duplicate timestamp");
    f=frame(data,1); check(!gpr_capture_submit(c.p,&f,UINT32_MAX),"submit after flush");
    check(!gpr_capture_finish(c.p) && !gpr_capture_finish(c.p),"idempotent finish");
    validate(sink,{reference(data),reference(data)});
}
static void errors() {
    auto data=image(4); auto f=frame(data,0);
    for (bool end : {false,true}) {
        Sink sink; sink.fail=!end; sink.fail_end=end;
        Capture c(config(),sink); check(!gpr_capture_submit(c.p,&f,UINT32_MAX),"error fixture submit");
        check(gpr_capture_finish(c.p)==GPR_CAPTURE_WRITER,"writer error missing");
        gpr_capture_stats stats={}; gpr_capture_get_stats(c.p,&stats);
        check(stats.error==GPR_CAPTURE_WRITER && stats.inflight==0,"failed work retained");
        check(stats.written+stats.discarded==stats.submitted,"error accounting");
    }
    Sink sink; sink.block=true; Capture c(config(1),sink);
    check(!gpr_capture_submit(c.p,&f,0),"cancel submit"); sink.wait();
    int result=0;
    auto next=frame(data,1);
    std::thread blocked([&]{ result=gpr_capture_submit(c.p,&next,UINT32_MAX); });
    gpr_capture_cancel(c.p); blocked.join(); check(result==GPR_CAPTURE_CANCELLED,"cancel did not wake submit");
    sink.unblock(); check(gpr_capture_finish(c.p)==GPR_CAPTURE_CANCELLED,"cancel finish");
}
static void concurrent_instances() {
    Sink first,second; Capture a(config(),first),b(config(),second);
    auto data=image(5); auto f=frame(data,0);
    std::thread one([&]{ check(!gpr_capture_submit(a.p,&f,UINT32_MAX),"concurrent one"); });
    std::thread two([&]{ check(!gpr_capture_submit(b.p,&f,UINT32_MAX),"concurrent two"); });
    std::vector<uint8_t> expected;
    std::thread sdk([&]{ expected=reference(data); }); // Ordinary SDK entry point in parallel with capture.
    one.join(); two.join(); sdk.join(); check(!gpr_capture_finish(a.p) && !gpr_capture_finish(b.p),"concurrent finish");
    validate(first,{expected}); validate(second,{expected});
}
struct Lease {
    std::mutex mutex;
    std::condition_variable changed;
    bool block=false,entered=false,ready=false;
    unsigned releases=0;
    std::thread::id worker;
    static void release(void *user) {
        Lease &lease=*static_cast<Lease *>(user);
        std::unique_lock<std::mutex> lock(lease.mutex);
        ++lease.releases; lease.worker=std::this_thread::get_id(); lease.entered=true; lease.changed.notify_all();
        if (lease.block) lease.changed.wait(lock,[&]{ return lease.ready; });
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock,std::chrono::seconds(10),[&]{ return entered; }),"lease gate timeout");
    }
    void unblock() { std::lock_guard<std::mutex> lock(mutex); ready=true; changed.notify_all(); }
};
static void leased_inputs() {
    {
        auto cfg=config(); cfg.stride_bytes=256;
        Sink sink; Capture c(cfg,sink); Lease lease;
        auto padded=image(6); std::vector<uint8_t> packed(128*96*2);
        for (unsigned y=0;y<96;++y) std::memcpy(packed.data()+y*256,padded.data()+y*272,256);
        auto f=frame(packed,0); f.release=Lease::release; f.release_user=&lease;
        check(!gpr_capture_submit(c.p,&f,0),"successful lease submit"); lease.wait();
        std::fill(packed.begin(),packed.end(),0); // Caller may reuse after release.
        check(!gpr_capture_finish(c.p) && lease.releases==1,"successful lease release");
        validate(sink,{reference(padded)});
    }
    auto cfg=config(); cfg.stride_bytes=256;
    Sink sink; Capture c(cfg,sink);
    std::vector<uint8_t> raw(128*96*2,0);
    auto f=frame(raw,0);
    Lease first,second,rejected; first.block=true;
    f.release=Lease::release; f.release_user=&first;
    check(!gpr_capture_submit(c.p,&f,0),"lease submit"); first.wait();
    f.index=1; ++f.timestamp_ns; f.release_user=&second;
    check(!gpr_capture_submit(c.p,&f,0),"queued lease submit");
    f.index=2; ++f.timestamp_ns; f.release_user=&rejected;
    check(gpr_capture_submit(c.p,&f,0)==GPR_CAPTURE_BUSY,"lease backpressure");
    gpr_capture_cancel(c.p); first.unblock();
    check(gpr_capture_finish(c.p)==GPR_CAPTURE_CANCELLED,"lease cancel finish");
    check(first.releases==1 && second.releases==1 && rejected.releases==0,"lease ownership on cancellation/rejection");
    check(first.worker==second.worker && first.worker!=std::this_thread::get_id(),"lease release thread");
    gpr_capture_stats stats={}; gpr_capture_get_stats(c.p,&stats);
    check(stats.inflight==0 && stats.discarded==2,"lease cancel accounting");
}
static void observer_lifetime() {
    Sink sink;sink.block_observer=true;
    auto cfg=config();Capture capture(cfg,sink);auto data=image(11);auto f=frame(data,0);
    check(!gpr_capture_submit(capture.p,&f,UINT32_MAX),"observer lifetime submit");
    {
        std::unique_lock<std::mutex> lock(sink.mutex);
        bool entered=sink.changed.wait_for(lock,std::chrono::seconds(10),[&]{return sink.observer_entered;});
        if (!entered) {sink.observer_released=true;sink.changed.notify_all();}
        check(entered,"observer gate timeout");
    }
    gpr_capture_cancel(capture.p);
    auto finishing=std::async(std::launch::async,[&]{return gpr_capture_finish(capture.p);});
    bool waits=finishing.wait_for(std::chrono::milliseconds(25))==std::future_status::timeout;
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        sink.observer_released=true;sink.changed.notify_all();
    }
    int result=finishing.get();
    check(waits,"finish returned while observer owned payload");
    check(result==GPR_CAPTURE_CANCELLED,"observer cancellation status");
    gpr_capture_stats state={};check(!gpr_capture_get_stats(capture.p,&state),"observer final stats");
    check(!state.inflight && !state.written && state.discarded==1,"observed cancellation accounting");
    check(sink.observed.size()==1 && !sink.completions,"observer/completion distinction");
}
static void formats() {
    for (bool gbrg : {false,true}) {
        auto cfg=config(); cfg.video.sample_bits=12;
        if (gbrg) std::memcpy(cfg.video.cfa,"GBRG",4);
        Sink sink; Capture c(cfg,sink); auto data=image(7);
        for (size_t y=0;y<96;++y) for (size_t x=0;x<128;++x) {
            uint16_t v; std::memcpy(&v,data.data()+y*272+x*2,2); v &= 4095;
            std::memcpy(data.data()+y*272+x*2,&v,2);
        }
        auto f=frame(data,0); check(!gpr_capture_submit(c.p,&f,UINT32_MAX),"12-bit submit");
        check(!gpr_capture_finish(c.p),"12-bit finish");
        check(get(sink.bytes.data()+36,4)==12,"sample depth metadata");
        check(!std::memcmp(sink.bytes.data()+32,gbrg ? "GBRG":"RGGB",4),"CFA metadata");
        validate(sink,{reference(data,gbrg ? VC5_ENCODER_PIXEL_FORMAT_GBRG_12:VC5_ENCODER_PIXEL_FORMAT_RGGB_12)});
    }
}
struct EncoderWait {
    gpr_capture *capture=nullptr;
    std::mutex mutex;std::condition_variable changed;
    bool entered=false,released=false;
    std::vector<uint64_t> indices;
    static void wait(void *user,uint64_t index) {
        auto &self=*static_cast<EncoderWait *>(user);gpr_capture_stats stats={};
        check(!gpr_capture_get_stats(self.capture,&stats) && stats.encoded>index,"encoder wait holds capture lock or precedes enqueue");
        std::unique_lock<std::mutex> lock(self.mutex);self.indices.push_back(index);
        self.entered=true;self.changed.notify_all();self.changed.wait(lock,[&]{return self.released;});
    }
    void entered_wait() {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock,std::chrono::seconds(10),[&]{return entered;}),"encoder barrier entry timeout");
    }
    void unblock() {std::lock_guard<std::mutex> lock(mutex);released=true;changed.notify_all();}
};
static void encoder_wait_barrier() {
    check(gpr_capture_set_encoder_wait(nullptr,nullptr,nullptr)==GPR_CAPTURE_INVALID,"null encoder barrier setter");
    Sink sink;EncoderWait barrier;Capture capture(config(),sink);barrier.capture=capture.p;
    check(!gpr_capture_set_encoder_wait(capture.p,EncoderWait::wait,&barrier),"encoder barrier setup");
    auto a=image(0),b=image(1);auto first=frame(a,0);
    check(!gpr_capture_submit(capture.p,&first,UINT32_MAX),"encoder barrier submit");barrier.entered_wait();
    bool written=false;auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do {
        gpr_capture_stats stats={};gpr_capture_get_stats(capture.p,&stats);written=stats.written==1;
        if (!written) std::this_thread::yield();
    } while (!written && std::chrono::steady_clock::now()<deadline);
    if (!written) {barrier.unblock();check(false,"writer cannot progress during encoder barrier");}
    int closed=gpr_capture_set_encoder_wait(capture.p,nullptr,nullptr);
    auto flushing=std::async(std::launch::async,[&]{return gpr_capture_flush(capture.p);});
    bool waited=flushing.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout;
    barrier.unblock();int result=flushing.get();
    check(waited && !result,"capture flush omitted active encoder barrier");
    check(closed==GPR_CAPTURE_CLOSED,"encoder barrier changed after submission");
    auto second=frame(b,1);check(!gpr_capture_submit(capture.p,&second,UINT32_MAX),"submit after barrier flush");
    check(!gpr_capture_finish(capture.p),"encoder barrier finish");
    check(barrier.indices==std::vector<uint64_t>({0,1}),"encoder barrier frame order");
    validate(sink,{reference(a),reference(b)});
}
int main(int argc,char **argv) {
    try {
        for (const char *flag : {"GPR_STREAM_UNPACK","GPR_BAND_STREAM","GPR_CHANNEL_ASM",
                 "GPR_PIN_CHANNELS","GPR_DIRECT_BANDS","GPR_COMPACT_BANDS"}) {
#if defined(_WIN32)
            _putenv_s(flag,"1");
#else
            setenv(flag,"1",1);
#endif
        }
        ownership(argc>1 ? argv[1] : nullptr); lifecycle(); errors(); concurrent_instances(); leased_inputs(); observer_lifetime(); formats();encoder_wait_barrier();
        std::cout<<"PASS: bounded capture, copy/lease ownership, padded stride, timestamps, stock payloads, flush/finish, writer failures, cancellation, concurrent SDK/capture\n";
        return 0;
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
