/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "gpr.h"
#include "vc5_encoder.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
static void check(bool yes,const char *why) { if (!yes) throw std::runtime_error(why); }
struct alignas(16) AllocationHeader { uint64_t magic; };
static size_t live_allocations=0;
static void *tracked_alloc(size_t size) {
    auto *p=static_cast<AllocationHeader *>(std::malloc(sizeof(AllocationHeader)+size));
    if (!p) return nullptr;
    p->magic=UINT64_C(0xF17EABCDEF123456); ++live_allocations; return p+1;
}
static void tracked_free(void *pointer) {
    if (!pointer) return;
    auto *p=static_cast<AllocationHeader *>(pointer)-1;
    if (p->magic!=UINT64_C(0xF17EABCDEF123456) || !live_allocations) std::abort();
    p->magic=0; --live_allocations; std::free(p);
}
struct Buffer {
    gpr_buffer value={};
    ~Buffer() { std::free(value.buffer); }
};
static void write(const char *path,const gpr_buffer &b) {
    std::ofstream out(path,std::ios::binary);
    check(bool(out.write(static_cast<const char *>(b.buffer),b.size)),"output write");
}
int main(int argc,char **argv) {
    try {
        check(argc==5,"format fixture.vc5 output.gpr output.dng");
        const unsigned bits[]={12,12,14,12,12,16,16,14,12,14,16,12,14,16};
        unsigned index=unsigned(std::atoi(argv[1])); check(index<14,"format index");
        std::ifstream in(argv[2],std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
        check(!bytes.empty(),"fixture read"); gpr_buffer input={bytes.data(),bytes.size()};
        gpr_allocator allocator={std::malloc,std::free};
        gpr_parameters p; gpr_parameters_set_defaults(&p);
        p.input_width=128; p.input_height=96;
        // Decode produces tightly packed words, independent of this source pitch.
        p.input_pitch=128*2+16;
        p.tuning_info.pixel_format=static_cast<GPR_PIXEL_FORMAT>(index);
        auto &white=p.tuning_info.dgain_saturation_level;
        white.level_red=white.level_green_even=white.level_green_odd=white.level_blue=(1u<<bits[index])-1;
        p.tuning_info.static_black_level={16,17,18,19};
        Buffer gpr,dng,extracted,gpr_raw,dng_raw;
        check(gpr_convert_vc5_to_gpr(&allocator,&p,&input,&gpr.value),"VC5 to GPR");
        check(gpr_convert_gpr_to_vc5(&allocator,&gpr.value,&extracted.value),"GPR to VC5");
        check(extracted.value.size==bytes.size() && !std::memcmp(extracted.value.buffer,bytes.data(),bytes.size()),"payload changed");
        check(gpr_convert_vc5_to_dng(&allocator,&p,&input,&dng.value),"VC5 to DNG");
        check(gpr_convert_gpr_to_raw(&allocator,&gpr.value,&gpr_raw.value),"GPR to raw");
        check(gpr_convert_dng_to_raw(&allocator,&dng.value,&dng_raw.value),"DNG to raw");
        check(gpr_raw.value.size==128*96*2 && dng_raw.value.size==gpr_raw.value.size &&
              !std::memcmp(gpr_raw.value.buffer,dng_raw.value.buffer,gpr_raw.value.size),"wrapper decoded pixels differ");
        Buffer fresh_vc5,fresh_gpr,fresh_extracted;
        check(gpr_convert_dng_to_vc5(&allocator,&dng.value,&fresh_vc5.value),"DNG to VC5");
        p.input_pitch=128*2;
        // Decoded DNG contains unpacked words, including for originally packed input.
        if (index==1) p.tuning_info.pixel_format=PIXEL_FORMAT_RGGB_12;
        if (index==4) p.tuning_info.pixel_format=PIXEL_FORMAT_GBRG_12;
        check(gpr_convert_raw_to_gpr(&allocator,&p,&dng_raw.value,&fresh_gpr.value),"fresh raw to GPR");
        check(gpr_convert_gpr_to_vc5(&allocator,&fresh_gpr.value,&fresh_extracted.value),"fresh GPR to VC5");
        check(fresh_vc5.value.size>4 && fresh_vc5.value.size==fresh_extracted.value.size &&
              !std::memcmp(fresh_vc5.value.buffer,fresh_extracted.value.buffer,fresh_vc5.value.size),"DNG to VC5 differs from fresh stock encode");
        gpr_buffer short_raw=dng_raw.value; --short_raw.size;
        Buffer rejected;
        check(!gpr_convert_raw_to_dng(&allocator,&p,&short_raw,&rejected.value) && !rejected.value.buffer,"short raw input accepted");
        p.enable_preview=false;
        for (unsigned q=0;q<6;++q) {
            Buffer reference, raw_quality, dng_quality, raw_payload, dng_payload;
            vc5_encoder_parameters encoding; vc5_encoder_parameters_set_default(&encoding);
            encoding.input_width=128; encoding.input_height=96; encoding.input_pitch=256;
            encoding.pixel_format=static_cast<VC5_ENCODER_PIXEL_FORMAT>(p.tuning_info.pixel_format);
            encoding.quality_setting=static_cast<VC5_ENCODER_QUALITY_SETTING>(q);
            check(!vc5_encoder_process(&encoding,&dng_raw.value,&reference.value,nullptr),"quality reference encode");
            p.fast_encoding=q%2==0; // Explicit selection must override the legacy flag.
            check(gpr_convert_raw_to_gpr_with_quality(&allocator,&p,q,&dng_raw.value,&raw_quality.value),"raw quality selection");
            check(gpr_convert_dng_to_gpr_with_quality(&allocator,&p,q,&dng.value,&dng_quality.value),"DNG quality selection");
            check(gpr_convert_gpr_to_vc5(&allocator,&raw_quality.value,&raw_payload.value) &&
                  gpr_convert_gpr_to_vc5(&allocator,&dng_quality.value,&dng_payload.value),"quality extraction");
            check(reference.value.size==raw_payload.value.size && reference.value.size==dng_payload.value.size &&
                  !std::memcmp(reference.value.buffer,raw_payload.value.buffer,reference.value.size) &&
                  !std::memcmp(reference.value.buffer,dng_payload.value.buffer,reference.value.size),"SDK preset differs from stock reference");
            if (q==1 || q==3) {
                Buffer legacy,payload; p.fast_encoding=q==1;
                check(gpr_convert_raw_to_gpr(&allocator,&p,&dng_raw.value,&legacy.value) &&
                      gpr_convert_gpr_to_vc5(&allocator,&legacy.value,&payload.value),"legacy encoding");
                check(payload.value.size==reference.value.size && !std::memcmp(payload.value.buffer,reference.value.buffer,reference.value.size),"legacy preset changed");
            }
        }
        check(!gpr_convert_raw_to_gpr_with_quality(&allocator,&p,6,&dng_raw.value,&rejected.value) &&
              !gpr_convert_dng_to_gpr_with_quality(&allocator,&p,6,&dng.value,&rejected.value) && !rejected.value.buffer,"invalid preset accepted");
        if (bits[index]<16) {
            uint16_t saved=static_cast<uint16_t *>(dng_raw.value.buffer)[0];
            static_cast<uint16_t *>(dng_raw.value.buffer)[0]=65535;
            check(!gpr_convert_raw_to_gpr_with_quality(&allocator,&p,3,&dng_raw.value,&rejected.value) &&
                  !rejected.value.buffer,"out-of-domain sample accepted");
            static_cast<uint16_t *>(dng_raw.value.buffer)[0]=saved;
        }
        if (index==2) {
            gpr_allocator tagged={tracked_alloc,tracked_free}; gpr_buffer custom={};
            p.enable_preview=true;
            check(gpr_convert_raw_to_gpr_with_quality(&tagged,&p,3,&dng_raw.value,&custom),"custom allocator encode");
            tracked_free(custom.buffer);
            check(live_allocations==0,"caller allocator ownership leaked");
        }
        write(argv[3],gpr.value); write(argv[4],dng.value);
        gpr_parameters_destroy(&p,std::free);
        std::cout<<"PASS: SDK payload wrapping and plain DNG, format "<<index<<'\n';
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
