/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_preview_queue.h"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using Clock=std::chrono::steady_clock;
uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
void release(const gpr_preview_plan *plan) { gpr_preview_plan_destroy(const_cast<gpr_preview_plan *>(plan)); }
bool background_priority() {
#if defined(__linux__)
    id_t self=id_t(syscall(SYS_gettid));errno=0;int priority=getpriority(PRIO_PROCESS,self);
    if (errno) return false;
    return setpriority(PRIO_PROCESS,self,std::max(priority,10))==0;
#elif defined(__APPLE__)
    qos_class_t current=qos_class_self();
    if (current!=QOS_CLASS_UNSPECIFIED && current<=QOS_CLASS_UTILITY) return true;
    return pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0)==0;
#elif defined(_WIN32)
    int current=GetThreadPriority(GetCurrentThread());
    if (current==THREAD_PRIORITY_ERROR_RETURN) return false;
    if (current<=THREAD_PRIORITY_BELOW_NORMAL) return true;
    return SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL)!=0;
#else
    return false;
#endif
}
}
struct gpr_preview_queue {
    struct Item {
        std::vector<uint8_t> payload;
        const gpr_preview_plan *plan=nullptr;
        uint64_t index=0,timestamp=0,offered=0;
        uint32_t wavelet_width=0,wavelet_height=0,payload_crc=0;
        uint32_t plane_stride_bytes=0;
    } slots[2];
    gpr_preview_queue_config config={};
    gpr_preview_queue_stats stats={};
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    std::vector<uint8_t> rgb;
    unsigned pending_slot=0;
    bool pending=false,active=false,closing=false,cancelled=false,done=false;
    uint64_t last_index=0,last_timestamp=0;
    size_t active_bytes=0;

    const gpr_preview_plan *discard_locked() {
        if (!pending) return nullptr;
        Item &item=slots[pending_slot];const auto *plan=item.plan;item.plan=nullptr;item.payload.clear();
        pending=false;++stats.discarded;--stats.inflight;return plan;
    }
    void run() noexcept {
        if (config.background) {
            bool applied=background_priority();std::lock_guard<std::mutex> lock(mutex);stats.background_applied=applied;
        }
        for (;;) {
            unsigned slot;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock,[&]{return pending || closing;});
                if (!pending) { done=true;changed.notify_all();return; }
                slot=pending_slot;pending_slot^=1;pending=false;active=true;active_bytes=slots[slot].payload.size();
            }
            Item &item=slots[slot];gpr_preview_frame_view view={};int error=0;bool displayed=false;
            uint64_t start=now_ns(),render_time=0,callback_time=0;
            try {
                error=gpr_preview_plan_info(item.plan,&view.image);
                if (!error) {
                    rgb.resize(view.image.row_bytes*view.image.height);
                    if (item.wavelet_width) {
                        gpr_preview_wavelet_planes planes={};planes.width=item.wavelet_width;planes.height=item.wavelet_height;
                        size_t count=size_t(planes.width)*planes.height;
                        size_t stride=item.plane_stride_bytes;
                        for (unsigned c=0;c<3;++c) {planes.samples[c]=reinterpret_cast<const int16_t *>(item.payload.data()+c*stride);planes.sample_count[c]=count;}
                        view.payload_crc32=item.payload_crc;
                        error=gpr_preview_plan_render_wavelets(item.plan,&planes,rgb.data(),rgb.size(),view.image.row_bytes,&view.image);
                    } else {
                        view.payload_crc32=gpr_gvid_crc32(0,item.payload.data(),item.payload.size());
                        error=gpr_preview_plan_render(item.plan,item.payload.data(),item.payload.size(),rgb.data(),rgb.size(),view.image.row_bytes,&view.image);
                    }
                }
                render_time=now_ns()-start;
                if (!error) {
                    bool deliver;
                    { std::lock_guard<std::mutex> lock(mutex);deliver=!cancelled; }
                    if (deliver) {
                        view.index=item.index;view.timestamp_ns=item.timestamp;view.offered_ns=item.offered;view.render_ns=render_time;
                        view.rgb=rgb.data();view.size_bytes=rgb.size();view.pitch=view.image.row_bytes;
                        start=now_ns();
                        try { if (config.display(config.user,&view)) error=GPR_PREVIEW_CALLBACK; }
                        catch (...) { error=GPR_PREVIEW_CALLBACK; }
                        callback_time=now_ns()-start;displayed=!error;
                    }
                }
            } catch (...) { error=GPR_PREVIEW_NOMEM; }
            const gpr_preview_plan *discard=nullptr,*processed=item.plan;item.plan=nullptr;
            {
                std::lock_guard<std::mutex> lock(mutex);
                active=false;active_bytes=0;--stats.inflight;stats.render_ns+=render_time;stats.callback_ns+=callback_time;
                if (displayed) ++stats.rendered;else ++stats.discarded;
                if (error) { if (!stats.error) stats.error=error;closing=true;discard=discard_locked(); }
                changed.notify_all();
            }
            release(processed);release(discard);
        }
    }
};
extern "C" int gpr_preview_queue_create(const gpr_preview_queue_config *c,gpr_preview_queue **out) {
    if (!out) return GPR_PREVIEW_INVALID;*out=nullptr;
    if (!c || !c->display || c->background>1 || !c->max_payload_bytes || c->max_payload_bytes>128*size_t(1024*1024) ||
        !c->max_rgb_bytes || c->max_rgb_bytes>128*size_t(1024*1024)) return GPR_PREVIEW_INVALID;
    try {
        std::unique_ptr<gpr_preview_queue> q(new gpr_preview_queue);q->config=*c;
        q->stats.background_requested=c->background;
        for (auto &slot:q->slots) slot.payload.reserve(c->max_payload_bytes);
        q->rgb.reserve(c->max_rgb_bytes);q->worker=std::thread(&gpr_preview_queue::run,q.get());
        *out=q.release();return 0;
    } catch (...) { return GPR_PREVIEW_NOMEM; }
}
static int offer(gpr_preview_queue *q,const gpr_preview_plan *plan,
    uint64_t index,uint64_t timestamp,const uint8_t *payload,size_t bytes,
    const gpr_preview_wavelet_planes *planes,uint32_t crc) {
    if (!q || !plan || (!payload && !planes) || !bytes || bytes>q->config.max_payload_bytes) return GPR_PREVIEW_INVALID;
    gpr_preview_info info={};
    if (gpr_preview_plan_info(plan,&info) || !info.height || info.row_bytes>q->config.max_rgb_bytes/info.height) return GPR_PREVIEW_CAPACITY;
    const gpr_preview_plan *old=nullptr;
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        if (q->stats.error) return q->stats.error;
        if (q->closing) return GPR_PREVIEW_CLOSED;
        if (q->stats.offered && (index<=q->last_index || timestamp<=q->last_timestamp)) return GPR_PREVIEW_INVALID;
        auto &item=q->slots[q->pending_slot];
        size_t storage_bytes=bytes,stride=bytes/3;
        if (planes) {
            // bytes is already bounded by the configured <=128 MiB reservation.
            // An odd multiple of 128 bytes gives the three packed planes
            // different low cache-set offsets instead of a power-of-two stride.
            // Spend only spare reserved bytes; keep dense storage at tight bounds.
            size_t candidate=(((stride+127)/128)|size_t(1))*128;
            if (candidate<=(q->config.max_payload_bytes-stride)/2) {
                storage_bytes=2*candidate+stride;stride=candidate;
            }
        }
        item.plane_stride_bytes=uint32_t(stride);
        item.payload.resize(storage_bytes);
        item.wavelet_width=planes ? planes->width:0;item.wavelet_height=planes ? planes->height:0;item.payload_crc=crc;
        if (planes) for (unsigned c=0;c<3;++c)
            std::memcpy(item.payload.data()+c*stride,planes->samples[c],bytes/3);
        else std::memcpy(item.payload.data(),payload,bytes);
        gpr_preview_plan_retain(plan);old=item.plan;item.plan=plan;
        item.index=index;item.timestamp=timestamp;item.offered=now_ns();
        if (q->pending) { ++q->stats.replaced;++q->stats.discarded; } else { ++q->stats.inflight;q->pending=true; }
        ++q->stats.offered;q->last_index=index;q->last_timestamp=timestamp;
        q->stats.inflight_high_water=std::max(q->stats.inflight_high_water,q->stats.inflight);
        q->stats.payload_peak_bytes=std::max(q->stats.payload_peak_bytes,uint64_t(q->active_bytes+storage_bytes));
        q->changed.notify_all();
    }
    release(old);return 0;
}
extern "C" int gpr_preview_queue_offer(gpr_preview_queue *q,const gpr_preview_plan *plan,
    uint64_t index,uint64_t timestamp,const uint8_t *payload,size_t bytes) {
    return offer(q,plan,index,timestamp,payload,bytes,nullptr,0);
}
extern "C" int gpr_preview_queue_offer_wavelets(gpr_preview_queue *q,const gpr_preview_plan *plan,
    uint64_t index,uint64_t timestamp,const gpr_preview_wavelet_planes *planes,uint32_t crc) {
    if (!planes || !planes->width || !planes->height || size_t(planes->width)>SIZE_MAX/planes->height/6)
        return GPR_PREVIEW_INVALID;
    size_t count=size_t(planes->width)*planes->height;
    for (unsigned c=0;c<3;++c) if (!planes->samples[c] || planes->sample_count[c]<count ||
        reinterpret_cast<uintptr_t>(planes->samples[c])%alignof(int16_t)) return GPR_PREVIEW_INVALID;
    return offer(q,plan,index,timestamp,nullptr,count*6,planes,crc);
}
extern "C" int gpr_preview_queue_get_stats(gpr_preview_queue *q,gpr_preview_queue_stats *out) {
    if (!q || !out) return GPR_PREVIEW_INVALID;
    std::lock_guard<std::mutex> lock(q->mutex);*out=q->stats;return 0;
}
extern "C" void gpr_preview_queue_cancel(gpr_preview_queue *q) {
    if (!q) return;const gpr_preview_plan *discard;
    { std::lock_guard<std::mutex> lock(q->mutex);q->closing=q->cancelled=true;discard=q->discard_locked();q->changed.notify_all(); }
    release(discard);
}
extern "C" int gpr_preview_queue_flush(gpr_preview_queue *q) {
    if (!q) return GPR_PREVIEW_INVALID;
    std::unique_lock<std::mutex> lock(q->mutex);
    q->changed.wait(lock,[&]{return !q->stats.inflight;});
    return q->stats.error;
}
extern "C" int gpr_preview_queue_finish(gpr_preview_queue *q) {
    if (!q) return GPR_PREVIEW_INVALID;
    std::unique_lock<std::mutex> lock(q->mutex);q->closing=true;q->changed.notify_all();q->changed.wait(lock,[&]{return q->done;});return q->stats.error;
}
extern "C" void gpr_preview_queue_destroy(gpr_preview_queue *q) {
    if (!q) return;gpr_preview_queue_cancel(q);gpr_preview_queue_finish(q);q->worker.join();delete q;
}
