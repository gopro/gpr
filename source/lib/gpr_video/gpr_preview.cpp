/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr_preview.h"
#include "vc5_decoder.h"
#include "logcurve.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <atomic>
#include <future>
#if defined(__aarch64__) && NEON
#include <arm_neon.h>
#endif

namespace {
VC5_DECODER_PIXEL_FORMAT decoder_format(const gpr_gvid_config &v) {
    static const VC5_DECODER_PIXEL_FORMAT formats[4][3] = {
        {VC5_DECODER_PIXEL_FORMAT_RGGB_12,VC5_DECODER_PIXEL_FORMAT_RGGB_14,VC5_DECODER_PIXEL_FORMAT_RGGB_16},
        {VC5_DECODER_PIXEL_FORMAT_GBRG_12,VC5_DECODER_PIXEL_FORMAT_GBRG_14,VC5_DECODER_PIXEL_FORMAT_GBRG_16},
        {VC5_DECODER_PIXEL_FORMAT_GRBG_12,VC5_DECODER_PIXEL_FORMAT_GRBG_14,VC5_DECODER_PIXEL_FORMAT_GRBG_16},
        {VC5_DECODER_PIXEL_FORMAT_BGGR_12,VC5_DECODER_PIXEL_FORMAT_BGGR_14,VC5_DECODER_PIXEL_FORMAT_BGGR_16}};
    unsigned phase=0;
    if (!std::memcmp(v.cfa,"GBRG",4)) phase=1;
    else if (!std::memcmp(v.cfa,"GRBG",4)) phase=2;
    else if (!std::memcmp(v.cfa,"BGGR",4)) phase=3;
    return formats[phase][(v.sample_bits-12)/2];
}
unsigned be16(const uint8_t *p) { return unsigned(p[0])*256+p[1]; }
bool stock_payload(const gpr_gvid_config &v,const uint8_t *p,size_t size) {
    if (!p || size<64 || size%4 || size>size_t(v.width)*v.height*3+1024*1024 || std::memcmp(p,"VC-5",4)) return false;
    size_t offset=4,ends[16]; unsigned depth=0,mask=0,channel=0,band=0,precision=16,count[4]={};
    while (offset<size) {
        while (depth && offset==ends[depth-1]) --depth;
        size_t end=depth ? ends[depth-1]:size;
        if (offset>end || end-offset<4) return false;
        unsigned word=be16(p+offset),value=be16(p+offset+2);
        int signed_tag=word<32768 ? int(word):int(word)-65536;
        unsigned tag=unsigned(signed_tag<0 ? -signed_tag:signed_tag); offset+=4;
        if (tag&0x6000) {
            size_t words=value+((tag&0x2000) ? size_t(tag&255)<<16:0);
            if (words>(end-offset)/4) return false;
            size_t next=offset+words*4;
            unsigned kind=tag&0xff00;
            if (kind==0x6000) {
                if (mask!=255 || band!=count[channel] || band>=10 || !words) return false;
                if (!band) {
                    size_t bits=size_t((v.width+15)/16)*((v.height+15)/16)*precision;
                    if (words!=(bits+31)/32) return false;
                }
                ++count[channel]; ++band; offset=next;
            } else if (kind==0x2000 || kind==0x2100 || kind==0x2400 || kind==0x2500 || kind==0x2600 || kind==0x2700) {
                if (depth==16 || !words) return false;
                ends[depth++]=next;
            } else {
                if (signed_tag>=0 || tag!=0x4004 || words!=9) return false;
                offset=next;
            }
            continue;
        }
        switch (tag) {
            case 12: if (value!=4) return false; mask|=1; break;
            case 20: if (value!=v.width) return false; mask|=2; break;
            case 21: if (value!=v.height) return false; mask|=4; break;
            case 84: if (value!=4) return false; mask|=8; break;
            case 106: if (value!=2) return false; mask|=16; break;
            case 107: if (value!=2) return false; mask|=32; break;
            case 108: if (value!=1) return false; mask|=64; break;
            case 102: if (value!=12) return false; mask|=128; break;
            case 101: if (value!=12) return false; break;
            case 14: if (value!=10) return false; break;
            case 62: if (value>=4) return false; channel=value; band=count[channel]; break;
            case 48: if (value!=count[channel] || value>=10) return false; band=value; break;
            case 35: if (value<1 || value>16) return false; precision=value; break;
            case 53: if (value>32767) return false; break;
            case 109: if (value!=0 && value!=0x2800) return false; break;
            default: return false;
        }
    }
    while (depth && offset==ends[depth-1]) --depth;
    return !depth && mask==255 && count[0]==10 && count[1]==10 && count[2]==10 && count[3]==10;
}
// The historical decoder has early error returns. Track frame allocations so
// wrapper failures also reclaim resources that did not reach its normal exit.
struct Allocations {
    std::map<void *,size_t> pointers;
    ~Allocations() { for (const auto &p:pointers) std::free(p.first); }
};
thread_local Allocations *active=nullptr;
void *allocate(size_t size) noexcept {
    void *p=std::malloc(size); if (!p) return nullptr;
    try { active->pointers.emplace(p,size); } catch (...) { std::free(p); return nullptr; }
    return p;
}
void release(void *p) {
    if (!p) return;
    auto found=active->pointers.find(p);
    if (found!=active->pointers.end()) { std::free(p); active->pointers.erase(found); }
}
}

struct gpr_preview_plan {
    mutable std::atomic<size_t> references{1};
    gpr_gvid_config video;
    uint32_t downsample,orientation,columns,rows,full_width,full_height;
    gpr_preview_info info;
    float black[4],matrix[9],camera_white[3],exposure,inverse_range[4],count_scale;
    unsigned red,green1,green2,blue;
    bool scalar;
    std::vector<uint32_t> xs,ys;
    std::vector<std::array<float,8>> gains;
    std::vector<std::array<float,4>> unity_cap_gains;
};
namespace {
std::array<float,8> pixel_gains(const gpr_preview_plan *p,size_t index) {
    if (!p->unity_cap_gains.empty()) {
        const auto &g=p->unity_cap_gains[index];
        return {{g[0],g[1],g[2],g[3],1,1,1,1}};
    }
    return p->gains.empty() ? std::array<float,8>{{1,1,1,1,1,1,1,1}}:p->gains[index];
}
bool rectangle(const float *r,const gpr_gvid_config &v) {
    for (unsigned i=0;i<4;++i) if (!std::isfinite(r[i])) return false;
    return r[0]>=0 && r[1]>=0 && r[2]>r[0] && r[3]>r[1] && r[2]<=v.width && r[3]<=v.height;
}
float map_gain(const gpr_preview_gain_map &m,float x,float y,const float *active) {
    float gx=((x+.5f-active[0])/(active[2]-active[0])-m.origin_x)/m.spacing_x;
    float gy=((y+.5f-active[1])/(active[3]-active[1])-m.origin_y)/m.spacing_y;
    gx=std::max(0.f,std::min(float(m.cols-1),gx)); gy=std::max(0.f,std::min(float(m.rows-1),gy));
    uint32_t left=uint32_t(gx),top=uint32_t(gy),right=std::min(left+1,m.cols-1),bottom=std::min(top+1,m.rows-1);
    float dx=gx-left,dy=gy-top;
    const float *p=m.values;
    return (1-dy)*((1-dx)*p[size_t(top)*m.cols+left]+dx*p[size_t(top)*m.cols+right])+
        dy*((1-dx)*p[size_t(bottom)*m.cols+left]+dx*p[size_t(bottom)*m.cols+right]);
}
const uint8_t *srgb_table() {
    static uint8_t table[65536]; static std::once_flag once;
    std::call_once(once,[]{ for (unsigned i=0;i<65536;++i) {
        double x=double(i)/65535;
        double y=x<=.0031308 ? 12.92*x:1.055*std::pow(x,1.0/2.4)-.055;
        table[i]=uint8_t(std::floor(y*255+.5));
    }});
    return table;
}
uint8_t srgb(float value,const uint8_t *table) {
    if (!(value>0)) return 0; if (value>=1) return 255;
    return table[unsigned(value*65535+.5f)];
}
void display_pixel(const gpr_preview_plan *p,uint8_t *rgb,size_t pitch,unsigned x,unsigned y,const uint8_t *color) {
    uint32_t dx=x,dy=y;
    switch (p->orientation) {
        case 2:dx=p->columns-1-x;break;
        case 3:dx=p->columns-1-x;dy=p->rows-1-y;break;
        case 4:dy=p->rows-1-y;break;
        case 5:dx=y;dy=x;break;
        case 6:dx=p->rows-1-y;dy=x;break;
        case 7:dx=p->rows-1-y;dy=p->columns-1-x;break;
        case 8:dx=y;dy=p->columns-1-x;break;
    }
    std::memcpy(rgb+size_t(dy)*pitch+size_t(dx)*3,color,3);
}
#if defined(__aarch64__) && NEON
void display_neon4(const gpr_preview_plan *p,const uint8_t *data,const uint8_t *gamma,uint8_t *rgb,size_t pitch,unsigned x,unsigned y) {
    // Four independent pixels keep the channel arithmetic in vector registers.
    // Load the mirrored big-endian word triplets once, then reverse lane order.
    const uint8_t *start=data+(size_t(p->ys[y])*p->full_width+(p->full_width-1-p->xs[x+3]))*6;
    uint16x4x3_t words=vld3_u16(reinterpret_cast<const uint16_t *>(start));
    float32x4_t samples[3];
    for (unsigned c=0;c<3;++c) {
        uint16x4_t value=vreinterpret_u16_u8(vrev16_u8(vreinterpret_u8_u16(words.val[c])));
        samples[c]=vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vrev64_u16(value))),1/p->count_scale);
    }
    float32x4_t gains[4],caps[4];
    if (!p->unity_cap_gains.empty()) {
        const float *values=p->unity_cap_gains[size_t(y)*p->columns+x].data();
        float32x4x4_t packed=vld4q_f32(values);
        for (unsigned site=0;site<4;++site) {gains[site]=packed.val[site];caps[site]=vdupq_n_f32(1);}
    } else if (p->gains.empty()) for (unsigned site=0;site<4;++site) gains[site]=caps[site]=vdupq_n_f32(1);
    else {
        const float *values=p->gains[size_t(y)*p->columns+x].data();
        float32x4x4_t first=vld4q_f32(values),last=vld4q_f32(values+16);
        for (unsigned site=0;site<4;++site) {
            gains[site]=vuzp1q_f32(first.val[site],last.val[site]);
            caps[site]=vuzp2q_f32(first.val[site],last.val[site]);
        }
    }
    auto corrected=[&](unsigned site,unsigned channel)->float32x4_t {
        float32x4_t value=vmulq_n_f32(vsubq_f32(samples[channel],vdupq_n_f32(p->black[site])),p->inverse_range[site]);
        value=vmaxq_f32(vdupq_n_f32(0),vminq_f32(vdupq_n_f32(1),value));
        return vminq_f32(caps[site],vmulq_f32(value,gains[site]));
    };
    float32x4_t camera[3]={corrected(p->red,0),vmulq_n_f32(vaddq_f32(corrected(p->green1,1),corrected(p->green2,1)),.5f),corrected(p->blue,2)};
    for (unsigned c=0;c<3;++c) camera[c]=vminq_f32(camera[c],vdupq_n_f32(p->camera_white[c]));
    uint32_t indices[3][4];
    for (unsigned c=0;c<3;++c) {
        float32x4_t value=vmulq_n_f32(camera[0],p->matrix[c*3]);
        value=vaddq_f32(value,vmulq_n_f32(camera[1],p->matrix[c*3+1]));
        value=vaddq_f32(value,vmulq_n_f32(camera[2],p->matrix[c*3+2]));
        value=vmulq_n_f32(vmaxq_f32(vdupq_n_f32(0),vminq_f32(vdupq_n_f32(1),value)),p->exposure);
        value=vmaxq_f32(vdupq_n_f32(0),vminq_f32(vdupq_n_f32(1),value));
        vst1q_u32(indices[c],vcvtq_u32_f32(vaddq_f32(vmulq_n_f32(value,65535),vdupq_n_f32(.5f))));
    }
    for (unsigned lane=0;lane<4;++lane) {
        uint8_t color[3]={gamma[indices[0][lane]],gamma[indices[1][lane]],gamma[indices[2][lane]]};
        display_pixel(p,rgb,pitch,x+lane,y,color);
    }
}
void display_wavelet_neon4(const gpr_preview_plan *p,const gpr_rgb_buffer *planes,
    const uint8_t *gamma,uint8_t *rgb,size_t pitch,unsigned x,unsigned y) {
    int32x4_t gs=vmovl_s16(vld1_s16(static_cast<const int16_t *>(planes[0].buffer)+size_t(p->ys[y])*p->full_width+p->xs[x]));
    int32x4_t rg=vmovl_s16(vld1_s16(static_cast<const int16_t *>(planes[1].buffer)+size_t(p->ys[y])*p->full_width+p->xs[x]));
    int32x4_t bg=vmovl_s16(vld1_s16(static_cast<const int16_t *>(planes[2].buffer)+size_t(p->ys[y])*p->full_width+p->xs[x]));
    int32x4_t midpoint=vdupq_n_s32(1<<13);
    int32x4_t indices[3]={
        vshrq_n_s32(vaddq_s32(vshlq_n_s32(vsubq_s32(rg,midpoint),1),gs),2),
        vshrq_n_s32(gs,2),
        vshrq_n_s32(vaddq_s32(vshlq_n_s32(vsubq_s32(bg,midpoint),1),gs),2)};
    float32x4_t samples[3];
    for (unsigned c=0;c<3;++c) {
        uint32_t index[4];uint16_t linear[4];
        vst1q_u32(index,vreinterpretq_u32_s32(vmaxq_s32(vdupq_n_s32(0),vminq_s32(vdupq_n_s32(4095),indices[c]))));
        for (unsigned lane=0;lane<4;++lane) linear[lane]=DecoderLogCurve[index[lane]];
        samples[c]=vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vld1_u16(linear))),1/p->count_scale);
    }
    float32x4_t gains[4],caps[4];
    if (!p->unity_cap_gains.empty()) {
        const float *values=p->unity_cap_gains[size_t(y)*p->columns+x].data();
        float32x4x4_t packed=vld4q_f32(values);
        for (unsigned site=0;site<4;++site) {gains[site]=packed.val[site];caps[site]=vdupq_n_f32(1);}
    } else if (p->gains.empty()) for (unsigned site=0;site<4;++site) gains[site]=caps[site]=vdupq_n_f32(1);
    else {
        const float *values=p->gains[size_t(y)*p->columns+x].data();
        float32x4x4_t first=vld4q_f32(values),last=vld4q_f32(values+16);
        for (unsigned site=0;site<4;++site) {
            gains[site]=vuzp1q_f32(first.val[site],last.val[site]);
            caps[site]=vuzp2q_f32(first.val[site],last.val[site]);
        }
    }
    auto corrected=[&](unsigned site,unsigned channel)->float32x4_t {
        float32x4_t value=vmulq_n_f32(vsubq_f32(samples[channel],vdupq_n_f32(p->black[site])),p->inverse_range[site]);
        value=vmaxq_f32(vdupq_n_f32(0),vminq_f32(vdupq_n_f32(1),value));
        return vminq_f32(caps[site],vmulq_f32(value,gains[site]));
    };
    float32x4_t camera[3]={corrected(p->red,0),vmulq_n_f32(vaddq_f32(corrected(p->green1,1),corrected(p->green2,1)),.5f),corrected(p->blue,2)};
    for (unsigned c=0;c<3;++c) camera[c]=vminq_f32(camera[c],vdupq_n_f32(p->camera_white[c]));
    uint32_t output[3][4];
    for (unsigned c=0;c<3;++c) {
        float32x4_t value=vmulq_n_f32(camera[0],p->matrix[c*3]);
        value=vaddq_f32(value,vmulq_n_f32(camera[1],p->matrix[c*3+1]));
        value=vaddq_f32(value,vmulq_n_f32(camera[2],p->matrix[c*3+2]));
        value=vmulq_n_f32(vmaxq_f32(vdupq_n_f32(0),vminq_f32(vdupq_n_f32(1),value)),p->exposure);
        value=vmaxq_f32(vdupq_n_f32(0),vminq_f32(vdupq_n_f32(1),value));
        vst1q_u32(output[c],vcvtq_u32_f32(vaddq_f32(vmulq_n_f32(value,65535),vdupq_n_f32(.5f))));
    }
    for (unsigned lane=0;lane<4;++lane) {
        uint8_t color[3]={gamma[output[0][lane]],gamma[output[1][lane]],gamma[output[2][lane]]};
        display_pixel(p,rgb,pitch,x+lane,y,color);
    }
}
#endif
void display_rows(const gpr_preview_plan *p,const uint8_t *data,const uint8_t *gamma,
    uint8_t *rgb,size_t pitch,uint32_t first,uint32_t last) {
    for (uint32_t y=first;y<last;++y) for (uint32_t x=0;x<p->columns;) {
#if defined(__aarch64__) && NEON
        if (!p->scalar && x+4<=p->columns && p->xs[x+1]==p->xs[x]+1 && p->xs[x+2]==p->xs[x]+2 && p->xs[x+3]==p->xs[x]+3) {
            display_neon4(p,data,gamma,rgb,pitch,x,y);x+=4;continue;
        }
#endif
        const uint8_t *pixel=data+(size_t(p->ys[y])*p->full_width+(p->full_width-1-p->xs[x]))*6;
        float camera[3];
        const std::array<float,8> gain=pixel_gains(p,size_t(y)*p->columns+x);
        auto corrected=[&](unsigned site,unsigned channel)->float {
            uint16_t encoded;std::memcpy(&encoded,pixel+channel*2,2);
            unsigned sample=(encoded>>8)|((encoded&255)<<8);
            float value=(sample/p->count_scale-p->black[site])*p->inverse_range[site];
            value=std::max(0.f,std::min(1.f,value));
            return std::min(gain[site+4],value*gain[site]);
        };
        camera[0]=corrected(p->red,0); camera[1]=.5f*(corrected(p->green1,1)+corrected(p->green2,1)); camera[2]=corrected(p->blue,2);
        for (unsigned c=0;c<3;++c) camera[c]=std::min(camera[c],p->camera_white[c]);
        uint8_t color[3];
        for (unsigned c=0;c<3;++c) {
            float value=p->matrix[c*3]*camera[0]+p->matrix[c*3+1]*camera[1]+p->matrix[c*3+2]*camera[2];
            color[c]=srgb(std::max(0.f,std::min(1.f,value))*p->exposure,gamma);
        }
        display_pixel(p,rgb,pitch,x,y,color);++x;
    }
}
void display_wavelet_rows(const gpr_preview_plan *p,const gpr_rgb_buffer *planes,
    const uint8_t *gamma,uint8_t *rgb,size_t pitch,uint32_t first,uint32_t last) {
    const auto *gs=static_cast<const int16_t *>(planes[0].buffer);
    const auto *rg=static_cast<const int16_t *>(planes[1].buffer);
    const auto *bg=static_cast<const int16_t *>(planes[2].buffer);
    for (uint32_t y=first;y<last;++y) for (uint32_t x=0;x<p->columns;) {
#if defined(__aarch64__) && NEON
        if (!p->scalar && x+4<=p->columns && p->xs[x+1]==p->xs[x]+1 && p->xs[x+2]==p->xs[x]+2 && p->xs[x+3]==p->xs[x]+3) {
            display_wavelet_neon4(p,planes,gamma,rgb,pitch,x,y);x+=4;continue;
        }
#endif
        size_t index=size_t(p->ys[y])*p->full_width+p->xs[x];
        int32_t values[3]={2*(int32_t(rg[index])-(1<<13))+gs[index],gs[index],
                           2*(int32_t(bg[index])-(1<<13))+gs[index]};
        float samples[3];
        for (unsigned c=0;c<3;++c) {
            int32_t value=values[c]>>2;
            samples[c]=DecoderLogCurve[std::max(0,std::min(4095,int(value)))]/p->count_scale;
        }
        const std::array<float,8> gain=pixel_gains(p,size_t(y)*p->columns+x);
        auto corrected=[&](unsigned site,unsigned channel)->float {
            float value=(samples[channel]-p->black[site])*p->inverse_range[site];
            value=std::max(0.f,std::min(1.f,value));
            return std::min(gain[site+4],value*gain[site]);
        };
        float camera[3]={corrected(p->red,0),.5f*(corrected(p->green1,1)+corrected(p->green2,1)),corrected(p->blue,2)};
        for (unsigned c=0;c<3;++c) camera[c]=std::min(camera[c],p->camera_white[c]);
        uint8_t color[3];
        for (unsigned c=0;c<3;++c) {
            float value=p->matrix[c*3]*camera[0]+p->matrix[c*3+1]*camera[1]+p->matrix[c*3+2]*camera[2];
            color[c]=srgb(std::max(0.f,std::min(1.f,value))*p->exposure,gamma);
        }
        display_pixel(p,rgb,pitch,x,y,color);++x;
    }
}
}
extern "C" void gpr_preview_calibration_identity(const gpr_gvid_config *v,gpr_preview_calibration *out) {
    if (!out) return; *out={};
    if (!v || (v->sample_bits!=12 && v->sample_bits!=14 && v->sample_bits!=16)) return;
    out->version=1; out->downsample=4; out->orientation=1; out->exposure=1;
    out->white=float((1u<<v->sample_bits)-1);
    out->camera_to_srgb[0]=out->camera_to_srgb[4]=out->camera_to_srgb[8]=1;
    out->camera_white[0]=out->camera_white[1]=out->camera_white[2]=1;
    out->active[2]=out->crop[2]=float(v->width); out->active[3]=out->crop[3]=float(v->height);
}
extern "C" int gpr_preview_plan_create(const gpr_gvid_config *v,const gpr_preview_calibration *c,gpr_preview_plan **out) {
    if (!out) return GPR_PREVIEW_INVALID; *out=nullptr;
    gpr_preview_options options={}; gpr_preview_options_default(&options);
    if (!v || !c || c->version!=1 || v->width<48 || v->height<48) return GPR_PREVIEW_INVALID;
    options.downsample=c->downsample; gpr_preview_info full={};
    if (gpr_preview_dimensions(v,&options,&full) || c->orientation<1 || c->orientation>8 ||
        !rectangle(c->active,*v) || !rectangle(c->crop,*v) ||
        c->crop[0]<c->active[0] || c->crop[1]<c->active[1] || c->crop[2]>c->active[2] || c->crop[3]>c->active[3] ||
        !std::isfinite(c->white) || c->white<1 || std::floor(c->white)!=c->white || c->white>((1u<<v->sample_bits)-1) ||
        !std::isfinite(c->exposure) || c->exposure<1.f/65536 || c->exposure>256 ||
        c->gain_map_count>32 || (c->gain_map_count && !c->gain_maps)) return GPR_PREVIEW_INVALID;
    for (float black:c->black) if (!std::isfinite(black) || black<0 || black>=c->white) return GPR_PREVIEW_INVALID;
    for (float entry:c->camera_to_srgb) if (!std::isfinite(entry) || std::abs(entry)>256) return GPR_PREVIEW_INVALID;
    for (float entry:c->camera_white) if (!std::isfinite(entry) || entry<=0 || entry>1) return GPR_PREVIEW_INVALID;
    for (size_t i=0;i<c->gain_map_count;++i) {
        const auto &m=c->gain_maps[i];
        if (!m.phase_mask || m.phase_mask>15 || !m.rows || !m.cols || m.rows>256 || m.cols>256 ||
            !m.values || m.value_count!=size_t(m.rows)*m.cols || !rectangle(m.area,*v) ||
            !std::isfinite(m.spacing_x) || !std::isfinite(m.spacing_y) || m.spacing_x<1e-12 || m.spacing_y<1e-12 ||
            !std::isfinite(m.origin_x) || !std::isfinite(m.origin_y) || std::abs(m.origin_x)>1e6 || std::abs(m.origin_y)>1e6)
            return GPR_PREVIEW_INVALID;
        for (size_t j=0;j<m.value_count;++j)
            if (!std::isfinite(m.values[j]) || m.values[j]<=0 || m.values[j]>64) return GPR_PREVIEW_INVALID;
    }
    try {
        std::unique_ptr<gpr_preview_plan> p(new gpr_preview_plan{});
        p->video=*v; p->downsample=c->downsample; p->orientation=c->orientation;
        const char *scalar=std::getenv("GPR_PREVIEW_SCALAR");p->scalar=scalar && !std::strcmp(scalar,"1");
        p->columns=uint32_t(std::ceil((c->crop[2]-c->crop[0])/c->downsample));
        p->rows=uint32_t(std::ceil((c->crop[3]-c->crop[1])/c->downsample));
        if (!p->columns || !p->rows) return GPR_PREVIEW_INVALID;
        p->full_width=full.width; p->full_height=full.height;
        p->info={p->orientation>=5 ? p->rows:p->columns,p->orientation>=5 ? p->columns:p->rows,0};
        p->info.row_bytes=size_t(p->info.width)*3;
        p->count_scale=float(1u<<(16-v->sample_bits));
        unsigned r=0,b=3;
        if (!std::memcmp(v->cfa,"GBRG",4)) { r=2; b=1; }
        else if (!std::memcmp(v->cfa,"GRBG",4)) { r=1; b=2; }
        else if (!std::memcmp(v->cfa,"BGGR",4)) { r=3; b=0; }
        unsigned green[2]={},count=0;
        for (unsigned i=0;i<4;++i) if (i!=r && i!=b) green[count++]=i;
        p->red=r; p->green1=green[0]; p->green2=green[1]; p->blue=b;
        for (unsigned i=0;i<4;++i) { p->black[i]=c->black[i]; p->inverse_range[i]=1/(c->white-p->black[i]); }
        for (unsigned i=0;i<9;++i) p->matrix[i]=c->camera_to_srgb[i];
        for (unsigned i=0;i<3;++i) p->camera_white[i]=c->camera_white[i];
        p->exposure=c->exposure;
        p->xs.resize(p->columns); p->ys.resize(p->rows);
        for (uint32_t x=0;x<p->columns;++x) p->xs[x]=std::min(full.width-1,uint32_t((c->crop[0]+(x+.5f)*(c->crop[2]-c->crop[0])/p->columns)/c->downsample));
        for (uint32_t y=0;y<p->rows;++y) p->ys[y]=std::min(full.height-1,uint32_t((c->crop[1]+(y+.5f)*(c->crop[3]-c->crop[1])/p->rows)/c->downsample));
        if (c->gain_map_count) {
            p->gains.resize(size_t(p->columns)*p->rows);
            bool caps_are_unity=true;
            for (uint32_t y=0;y<p->rows;++y) for (uint32_t x=0;x<p->columns;++x) {
                float sx=c->crop[0]+(x+.5f)*(c->crop[2]-c->crop[0])/p->columns-.5f;
                float sy=c->crop[1]+(y+.5f)*(c->crop[3]-c->crop[1])/p->rows-.5f;
                float sites[4]={1,1,1,1},caps[4]={1,1,1,1};
                for (size_t i=0;i<c->gain_map_count;++i) {
                    const auto &map=c->gain_maps[i];
                    if (sx<map.area[0] || sy<map.area[1] || sx>=map.area[2] || sy>=map.area[3]) continue;
                    float gain=map_gain(map,sx,sy,c->active);
                    for (unsigned site=0;site<4;++site) if (map.phase_mask&(1u<<site)) {
                        sites[site]*=gain; caps[site]=std::min(1.f,caps[site]*gain);
                    }
                }
                for (float gain:sites) if (!std::isfinite(gain) || gain>65536) return GPR_PREVIEW_INVALID;
                for (float cap:caps) if (cap!=1.f) caps_are_unity=false;
                p->gains[size_t(y)*p->columns+x]={{sites[0],sites[1],sites[2],sites[3],caps[0],caps[1],caps[2],caps[3]}};
            }
            if (caps_are_unity) {
                // Preserve evaluated float values exactly. Common lens gains
                // never lower the sequential clip cap: store four gains rather
                // than reading four additional constant caps for every pixel.
                p->unity_cap_gains.resize(p->gains.size());
                for (size_t i=0;i<p->gains.size();++i)
                    std::copy(p->gains[i].begin(),p->gains[i].begin()+4,p->unity_cap_gains[i].begin());
                std::vector<std::array<float,8>>().swap(p->gains);
            }
        }
        *out=p.release(); return GPR_PREVIEW_OK;
    } catch (...) { return GPR_PREVIEW_NOMEM; }
}
extern "C" int gpr_preview_plan_info(const gpr_preview_plan *p,gpr_preview_info *out) {
    if (!p || !out) return GPR_PREVIEW_INVALID; *out=p->info; return GPR_PREVIEW_OK;
}
extern "C" int gpr_preview_plan_from_profile(const gpr_gvid_config *v,const uint8_t *data,size_t size,gpr_preview_plan **out) {
    if (!out) return GPR_PREVIEW_INVALID; *out=nullptr;
    if (!v || !data || size<144 || size>16*size_t(1024*1024) || std::memcmp(data,"GPRP",4)) return GPR_PREVIEW_INVALID;
    auto number=[&](size_t at)->uint32_t { return uint32_t(data[at])|uint32_t(data[at+1])<<8|uint32_t(data[at+2])<<16|uint32_t(data[at+3])<<24; };
    auto real=[&](size_t at)->float { uint32_t bits=number(at); float f; std::memcpy(&f,&bits,4); return f; };
    if (number(4)!=1 || number(8)!=v->width || number(12)!=v->height || number(16)!=v->sample_bits ||
        std::memcmp(data+20,v->cfa,4) || number(136)>32 || gpr_gvid_crc32(0,data,size-4)!=number(size-4)) return GPR_PREVIEW_INVALID;
    try {
        gpr_preview_calibration c={}; c.version=1; c.downsample=number(24); c.orientation=number(28);
        for (unsigned i=0;i<4;++i) c.black[i]=real(32+i*4);
        c.white=real(48);
        for (unsigned i=0;i<9;++i) c.camera_to_srgb[i]=real(52+i*4);
        for (unsigned i=0;i<3;++i) c.camera_white[i]=real(88+i*4);
        c.exposure=real(100);
        for (unsigned i=0;i<4;++i) { c.active[i]=real(104+i*4); c.crop[i]=real(120+i*4); }
        size_t position=140,count=number(136);
        std::vector<gpr_preview_gain_map> maps(count);
        std::vector<std::vector<float>> grids(count);
        for (size_t i=0;i<count;++i) {
            if (position>size-4 || size-4-position<44) return GPR_PREVIEW_INVALID;
            auto &m=maps[i]; m.phase_mask=number(position); m.rows=number(position+4); m.cols=number(position+8);
            if (!m.rows || !m.cols || m.rows>256 || m.cols>256) return GPR_PREVIEW_INVALID;
            m.spacing_y=real(position+12); m.spacing_x=real(position+16); m.origin_y=real(position+20); m.origin_x=real(position+24);
            for (unsigned j=0;j<4;++j) m.area[j]=real(position+28+j*4);
            position+=44; m.value_count=size_t(m.rows)*m.cols;
            if (m.value_count>(size-4-position)/4) return GPR_PREVIEW_INVALID;
            auto &values=grids[i]; values.resize(m.value_count);
            for (size_t j=0;j<m.value_count;++j) values[j]=real(position+j*4);
            position+=m.value_count*4; m.values=values.data();
        }
        if (position!=size-4) return GPR_PREVIEW_INVALID;
        c.gain_maps=maps.data(); c.gain_map_count=count;
        return gpr_preview_plan_create(v,&c,out);
    } catch (...) { return GPR_PREVIEW_NOMEM; }
}
extern "C" void gpr_preview_plan_retain(const gpr_preview_plan *p) { if (p) p->references.fetch_add(1,std::memory_order_relaxed); }
extern "C" void gpr_preview_plan_destroy(gpr_preview_plan *p) {
    if (p && p->references.fetch_sub(1,std::memory_order_acq_rel)==1) delete p;
}
extern "C" int gpr_preview_plan_render_wavelets(const gpr_preview_plan *p,
    const gpr_preview_wavelet_planes *input,uint8_t *rgb,size_t capacity,size_t pitch,gpr_preview_info *out) {
    if (!p || !input || !rgb || !out || input->width!=p->full_width || input->height!=p->full_height)
        return GPR_PREVIEW_INVALID;
    if (pitch<p->info.row_bytes || pitch>SIZE_MAX/p->info.height ||
        capacity<(p->info.height-1)*pitch+p->info.row_bytes) return GPR_PREVIEW_CAPACITY;
    const size_t count=size_t(p->full_width)*p->full_height;
    gpr_rgb_buffer planes[3]={};
    for (unsigned c=0;c<3;++c) {
        if (!input->samples[c] || reinterpret_cast<uintptr_t>(input->samples[c])%alignof(int16_t) ||
            input->sample_count[c]<count) return GPR_PREVIEW_INVALID;
        planes[c].buffer=const_cast<int16_t *>(input->samples[c]);
    }
    try {
        SetupDecoderLogCurve();
        const uint8_t *gamma=srgb_table();
        if (size_t(p->columns)*p->rows>=500000) {
            constexpr uint32_t workers=4,chunk_rows=32;
            std::atomic<uint32_t> next{0};
            auto render_chunks=[&] {
                for (;;) {
                    uint32_t first=next.fetch_add(chunk_rows,std::memory_order_relaxed);
                    if (first>=p->rows) return;
                    display_wavelet_rows(p,planes,gamma,rgb,pitch,first,std::min(p->rows,first+chunk_rows));
                }
            };
            // Keep the claim counter alive until every future has joined, also
            // when launching a later worker throws. Output row ranges are disjoint.
            std::future<void> done[workers-1];
            for (uint32_t i=1;i<workers;++i)
                done[i-1]=std::async(std::launch::async,render_chunks);
            render_chunks();
            for (auto &worker:done) worker.get();
        } else display_wavelet_rows(p,planes,gamma,rgb,pitch,0,p->rows);
        *out=p->info;return GPR_PREVIEW_OK;
    } catch (...) { return GPR_PREVIEW_NOMEM; }
}
extern "C" int gpr_preview_plan_render(const gpr_preview_plan *p,const uint8_t *payload,size_t size,
    uint8_t *rgb,size_t capacity,size_t pitch,gpr_preview_info *out) {
    if (!p || !rgb || !out) return GPR_PREVIEW_INVALID;
    if (pitch<p->info.row_bytes || pitch>SIZE_MAX/p->info.height ||
        capacity<(p->info.height-1)*pitch+p->info.row_bytes) return GPR_PREVIEW_CAPACITY;
    if (!stock_payload(p->video,payload,size)) return GPR_PREVIEW_PAYLOAD;
    try {
        std::vector<uint8_t> aligned;
        if (reinterpret_cast<uintptr_t>(payload)%4) { aligned.assign(payload,payload+size); payload=aligned.data(); }
        Allocations allocation; auto previous=active; active=&allocation;
        struct Restore { Allocations *old; ~Restore() { active=old; } } restore{previous};
        vc5_decoder_parameters parameters={}; vc5_decoder_parameters_set_default(&parameters);
        parameters.mem_alloc=allocate; parameters.mem_free=release;
        parameters.pixel_format=decoder_format(p->video);
        parameters.rgb_resolution=p->downsample==4 ? GPR_RGB_RESOLUTION_QUARTER:
            (p->downsample==8 ? GPR_RGB_RESOLUTION_EIGHTH:GPR_RGB_RESOLUTION_SIXTEENTH);
        parameters.rgb_bits=16;
        gpr_buffer input={const_cast<uint8_t *>(payload),size}; gpr_rgb_buffer decoded={};
        gpr_rgb_buffer planes[3]={};
        struct PlaneRelease {
            gpr_rgb_buffer *planes;
            ~PlaneRelease() { for (unsigned i=0;i<3;++i) std::free(planes[i].buffer); }
        } plane_release{planes};
        CODEC_ERROR error=CODEC_ERROR_OKAY;
        const char *serial=std::getenv("GPR_PREVIEW_SERIAL_DECODER");
        const bool serial_decoder=serial && !std::strcmp(serial,"1");
        if (serial_decoder) {
            error=vc5_decoder_process(&parameters,&input,nullptr,&decoded);
        } else {
            CODEC_ERROR errors[3]={CODEC_ERROR_OKAY,CODEC_ERROR_OKAY,CODEC_ERROR_OKAY};
            vc5_decoder_parameters channel_parameters=parameters;
            channel_parameters.mem_alloc=std::malloc;channel_parameters.mem_free=std::free;
            auto decode_channel=[&](unsigned channel) {
                return vc5_decoder_process_wavelet_channel(&channel_parameters,&input,channel,&planes[channel]);
            };
            auto channel1=std::async(std::launch::async,decode_channel,1);
            auto channel2=std::async(std::launch::async,decode_channel,2);
            errors[0]=decode_channel(0);errors[1]=channel1.get();errors[2]=channel2.get();
            for (CODEC_ERROR channel_error:errors) if (!error && channel_error) error=channel_error;
        }
        if (error) return GPR_PREVIEW_CODEC;
        if (serial_decoder) {
            if (!decoded.buffer || decoded.width!=p->full_width || decoded.height!=p->full_height ||
                decoded.size!=size_t(p->full_width)*p->full_height*6) return GPR_PREVIEW_CODEC;
        } else for (const auto &plane:planes) {
            if (!plane.buffer || plane.width!=p->full_width || plane.height!=p->full_height ||
                plane.size!=size_t(p->full_width)*p->full_height*sizeof(int16_t)) return GPR_PREVIEW_CODEC;
        }
        if (!serial_decoder) {
            gpr_preview_wavelet_planes input={};input.width=p->full_width;input.height=p->full_height;
            for (unsigned c=0;c<3;++c) {
                input.samples[c]=static_cast<const int16_t *>(planes[c].buffer);
                input.sample_count[c]=planes[c].size/sizeof(int16_t);
            }
            return gpr_preview_plan_render_wavelets(p,&input,rgb,capacity,pitch,out);
        }
        const uint8_t *data=static_cast<const uint8_t *>(decoded.buffer);
        const uint8_t *gamma=srgb_table();
        if (size_t(p->columns)*p->rows>=500000) {
            constexpr uint32_t workers=4;
            std::future<void> workers_done[workers-1];
            for (uint32_t i=1;i<workers;++i) {
                uint32_t first=p->rows*i/workers,last=p->rows*(i+1)/workers;
                workers_done[i-1]=std::async(std::launch::async,display_rows,p,data,gamma,rgb,pitch,first,last);
            }
            display_rows(p,data,gamma,rgb,pitch,0,p->rows/workers);
            for (auto &worker:workers_done) worker.get();
        } else {
            display_rows(p,data,gamma,rgb,pitch,0,p->rows);
        }
        release(decoded.buffer); *out=p->info; return GPR_PREVIEW_OK;
    } catch (...) { return GPR_PREVIEW_NOMEM; }
}
extern "C" void gpr_preview_options_default(gpr_preview_options *out) {
    if (out) {
        *out={}; out->downsample=4;
        gpr_rgb_gain gain; gpr_rgb_gain_set_defaults(&gain);
        out->gain_num[0]=gain.r_gain_num; out->gain_pow2_den[0]=gain.r_gain_pow2_den;
        out->gain_num[1]=gain.g_gain_num; out->gain_pow2_den[1]=gain.g_gain_pow2_den;
        out->gain_num[2]=gain.b_gain_num; out->gain_pow2_den[2]=gain.b_gain_pow2_den;
    }
}
extern "C" int gpr_preview_validate_payload(const gpr_gvid_config *video,const uint8_t *payload,size_t size) {
    uint8_t header[64];
    if (!video || video->width<48 || video->height<48 || gpr_gvid_write_header(header,video,nullptr,0))
        return GPR_PREVIEW_INVALID;
    return stock_payload(*video,payload,size) ? GPR_PREVIEW_OK:GPR_PREVIEW_PAYLOAD;
}
extern "C" int gpr_preview_dimensions(const gpr_gvid_config *v,const gpr_preview_options *o,gpr_preview_info *out) {
    uint8_t check[64];
    if (!v || !o || !out || v->width<48 || v->height<48 || gpr_gvid_write_header(check,v,nullptr,0) ||
        (o->downsample!=4 && o->downsample!=8 && o->downsample!=16)) return GPR_PREVIEW_INVALID;
    for (unsigned i=0;i<3;++i) if (!o->gain_num[i] || o->gain_num[i]>1024 || o->gain_pow2_den[i]>16) return GPR_PREVIEW_INVALID;
    out->width=(v->width+o->downsample-1)/o->downsample;
    out->height=(v->height+o->downsample-1)/o->downsample;
    out->row_bytes=size_t(out->width)*3; return 0;
}
extern "C" int gpr_preview_render(const gpr_gvid_config *v,const gpr_preview_options *o,
    const uint8_t *payload,size_t size,uint8_t *rgb,size_t capacity,size_t pitch,gpr_preview_info *out) {
    gpr_preview_info dimensions={};
    if (!out || !rgb || gpr_preview_dimensions(v,o,&dimensions)) return GPR_PREVIEW_INVALID;
    if (pitch<dimensions.row_bytes || pitch>SIZE_MAX/dimensions.height ||
        capacity<(dimensions.height-1)*pitch+dimensions.row_bytes) return GPR_PREVIEW_CAPACITY;
    if (!stock_payload(*v,payload,size)) return GPR_PREVIEW_PAYLOAD;
    try {
        std::vector<uint8_t> aligned;
        if (reinterpret_cast<uintptr_t>(payload)%4) { aligned.assign(payload,payload+size); payload=aligned.data(); }
        Allocations allocation; auto previous=active; active=&allocation;
        struct Restore { Allocations *old; ~Restore() { active=old; } } restore{previous};
        vc5_decoder_parameters parameters={}; vc5_decoder_parameters_set_default(&parameters);
        parameters.mem_alloc=allocate; parameters.mem_free=release;
        parameters.pixel_format=decoder_format(*v);
        parameters.rgb_resolution=o->downsample==4 ? GPR_RGB_RESOLUTION_QUARTER:
            (o->downsample==8 ? GPR_RGB_RESOLUTION_EIGHTH:GPR_RGB_RESOLUTION_SIXTEENTH);
        parameters.rgb_bits=8;
        parameters.rgb_gain={int(o->gain_num[0]),int(o->gain_pow2_den[0]),int(o->gain_num[1]),
            int(o->gain_pow2_den[1]),int(o->gain_num[2]),int(o->gain_pow2_den[2])};
        gpr_buffer input={const_cast<uint8_t *>(payload),size}; gpr_rgb_buffer decoded={};
        CODEC_ERROR error=vc5_decoder_process(&parameters,&input,nullptr,&decoded);
        if (error || !decoded.buffer || decoded.width!=dimensions.width || decoded.height!=dimensions.height ||
            decoded.size!=dimensions.row_bytes*dimensions.height) return GPR_PREVIEW_CODEC;
        for (size_t y=0;y<dimensions.height;++y)
            std::memcpy(rgb+y*pitch,static_cast<uint8_t *>(decoded.buffer)+y*dimensions.row_bytes,dimensions.row_bytes);
        release(decoded.buffer); *out=dimensions; return 0;
    } catch (...) { return GPR_PREVIEW_NOMEM; }
}
