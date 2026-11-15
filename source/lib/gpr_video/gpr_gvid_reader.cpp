/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_gvid_reader.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <zlib.h>
namespace {
uint64_t le(const uint8_t *p,unsigned bytes) {
    uint64_t result=0; for (unsigned i=0;i<bytes;++i) result|=uint64_t(p[i])<<(i*8); return result;
}
struct Error { int code; };
void require(bool okay,int error=GPR_GVID_INVALID) { if (!okay) throw Error{error}; }
}
struct gpr_gvid_reader {
    gpr_gvid_read_fn read;
    void *user;
    size_t max_payload;
    gpr_gvid_stream_info info={};
    std::vector<uint8_t> stream_metadata,metadata,payload;
    uint64_t index=0,last_timestamp=0,payload_bytes=0;
    uint32_t crc=0;
    int status=0;
    bool verified=false;
    bool exact(uint8_t *out,size_t size,bool eof_allowed=false) {
        size_t done=0;
        while (done<size) {
            ptrdiff_t got=read(user,out+done,size-done);
            require(got>=0,GPR_GVID_IO);
            require(size_t(got)<=size-done,GPR_GVID_IO);
            if (!got) {
                if (!done && eof_allowed) return false;
                throw Error{GPR_GVID_TRUNCATED};
            }
            done+=size_t(got);
        }
        return true;
    }
    void header() {
        uint8_t h[64]; exact(h,64);
        require(!std::memcmp(h,"GVID",4) && le(h+6,2)==64 && !std::memcmp(h+8,"VC5\0",4));
        info.version=uint32_t(le(h+4,2)); require(info.version==2 || info.version==3);
        auto &v=info.video;
        v.width=uint32_t(le(h+12,4)); v.height=uint32_t(le(h+16,4));
        v.fps_num=uint32_t(le(h+20,4)); v.fps_den=uint32_t(le(h+24,4));
        v.quality=uint32_t(le(h+28,4)); std::memcpy(v.cfa,h+32,4); v.sample_bits=uint32_t(le(h+36,4));
        uint8_t check[64]; require(!gpr_gvid_write_header(check,&v,nullptr,0));
        if (info.version==2) {
            require(!std::memcmp(v.cfa,"RGGB",4) && v.sample_bits==14 && v.fps_den==1 &&
                    v.fps_num<=240 && le(h+40,4)==32);
            for (unsigned i=44;i<64;++i) require(h[i]==0);
        } else {
            require(le(h+40,4)==48 && le(h+44,4)<=GPR_GVID_METADATA_LIMIT && le(h+48,4)==1 &&
                    le(h+56,4)==gpr_gvid_crc32(0,h,56) && !le(h+60,4));
            stream_metadata.resize(size_t(le(h+44,4)));
            exact(stream_metadata.data(),stream_metadata.size());
            require(gpr_gvid_crc32(0,stream_metadata.data(),stream_metadata.size())==le(h+52,4));
            crc=gpr_gvid_crc32(gpr_gvid_crc32(0,h,64),stream_metadata.data(),stream_metadata.size());
        }
        info.metadata=stream_metadata.data(); info.metadata_size=stream_metadata.size();
        max_payload=std::min(max_payload,size_t(v.width)*v.height*8);
    }
    int next(gpr_gvid_frame_view *out) {
        if (status) return status;
        uint8_t h[48]; size_t record=info.version==3 ? 48:32;
        if (!exact(h,record,info.version==2)) {
            require(index!=0); return status=GPR_GVID_DONE;
        }
        if (info.version==3 && !std::memcmp(h,"END3",4)) {
            require(le(h+4,4)==48 && le(h+8,8)==index && le(h+16,8)==last_timestamp &&
                    le(h+24,8)==payload_bytes && le(h+32,4)==crc &&
                    le(h+36,4)==gpr_gvid_crc32(0,h,36) && !le(h+40,8));
            uint8_t byte; require(!exact(&byte,1,true));
            verified=true; return status=GPR_GVID_DONE;
        }
        require(index<1000000,GPR_GVID_LIMIT);
        require(!std::memcmp(h,info.version==3 ? "FRM3":"FRM2",4) && le(h+4,4)==record && le(h+8,8)==index);
        uint64_t timestamp=le(h+16,8),size=le(h+24,8);
        require(size>0 && size<=max_payload,GPR_GVID_LIMIT);
        size_t meta_size=0;
        if (info.version==3) {
            require(le(h+44,4)==gpr_gvid_crc32(0,h,44) && (!index || timestamp>last_timestamp));
            meta_size=size_t(le(h+32,4)); require(meta_size<=GPR_GVID_METADATA_LIMIT,GPR_GVID_LIMIT);
        } else require(timestamp==index*1000000000ULL/info.video.fps_num);
        metadata.resize(meta_size); exact(metadata.data(),metadata.size());
        payload.resize(size_t(size)); exact(payload.data(),payload.size());
        if (info.version==3) {
            uint32_t payload_crc=gpr_gvid_crc32(0,payload.data(),payload.size());
            require(payload_crc==le(h+36,4) && gpr_gvid_crc32(0,metadata.data(),metadata.size())==le(h+40,4));
            crc=gpr_gvid_crc32(gpr_gvid_crc32(crc,h,48),metadata.data(),metadata.size());
            crc=uint32_t(crc32_combine(crc,payload_crc,z_off_t(payload.size())));
        }
        *out={index,timestamp,payload.data(),metadata.data(),payload.size(),metadata.size()};
        ++index; last_timestamp=timestamp; payload_bytes+=size;
        return GPR_GVID_FRAME;
    }
};
extern "C" int gpr_gvid_reader_create(gpr_gvid_read_fn read,void *user,size_t limit,gpr_gvid_reader **out) {
    if (!out) return GPR_GVID_INVALID;
    *out=nullptr;
    if (!read || limit>512*size_t(1024*1024)) return GPR_GVID_INVALID;
    try {
        std::unique_ptr<gpr_gvid_reader> r(new gpr_gvid_reader);
        r->read=read; r->user=user; r->max_payload=limit ? limit:128*size_t(1024*1024);
        r->header(); *out=r.release(); return 0;
    } catch (const Error &error) { return error.code; }
      catch (const std::bad_alloc &) { return GPR_GVID_NOMEM; }
      catch (...) { return GPR_GVID_IO; }
}
extern "C" int gpr_gvid_reader_info(gpr_gvid_reader *r,gpr_gvid_stream_info *out) {
    if (!r || !out) return GPR_GVID_INVALID; *out=r->info; return 0;
}
extern "C" int gpr_gvid_reader_next(gpr_gvid_reader *r,gpr_gvid_frame_view *out) {
    if (!r || !out) return GPR_GVID_INVALID;
    std::memset(out,0,sizeof(*out));
    try { return r->next(out); }
    catch (const Error &error) { return r->status=error.code; }
    catch (const std::bad_alloc &) { return r->status=GPR_GVID_NOMEM; }
    catch (...) { return r->status=GPR_GVID_IO; }
}
extern "C" int gpr_gvid_reader_end_verified(gpr_gvid_reader *r) { return r && r->verified ? 1:0; }
extern "C" void gpr_gvid_reader_close(gpr_gvid_reader *r) { delete r; }
