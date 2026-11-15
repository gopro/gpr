/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#include "vc5_encoder.h"
#include "vc5_decoder.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
static void check(bool yes,const char *why) { if (!yes) throw std::runtime_error(why); }
struct Format {
    VC5_ENCODER_PIXEL_FORMAT encoder;
    VC5_DECODER_PIXEL_FORMAT decoder;
    unsigned bits;
    // Canonical R,G1,G2,B positions in each physical 2x2 tile.
    std::array<unsigned,4> positions;
};
static const Format formats[] = {
    {VC5_ENCODER_PIXEL_FORMAT_RGGB_12,VC5_DECODER_PIXEL_FORMAT_RGGB_12,12,{{0,1,2,3}}},
    {VC5_ENCODER_PIXEL_FORMAT_RGGB_14,VC5_DECODER_PIXEL_FORMAT_RGGB_14,14,{{0,1,2,3}}},
    {VC5_ENCODER_PIXEL_FORMAT_RGGB_16,VC5_DECODER_PIXEL_FORMAT_RGGB_16,16,{{0,1,2,3}}},
    {VC5_ENCODER_PIXEL_FORMAT_GBRG_12,VC5_DECODER_PIXEL_FORMAT_GBRG_12,12,{{2,0,3,1}}},
    {VC5_ENCODER_PIXEL_FORMAT_GBRG_14,VC5_DECODER_PIXEL_FORMAT_GBRG_14,14,{{2,0,3,1}}},
    {VC5_ENCODER_PIXEL_FORMAT_GBRG_16,VC5_DECODER_PIXEL_FORMAT_GBRG_16,16,{{2,0,3,1}}},
    {VC5_ENCODER_PIXEL_FORMAT_GRBG_12,VC5_DECODER_PIXEL_FORMAT_GRBG_12,12,{{1,0,3,2}}},
    {VC5_ENCODER_PIXEL_FORMAT_GRBG_14,VC5_DECODER_PIXEL_FORMAT_GRBG_14,14,{{1,0,3,2}}},
    {VC5_ENCODER_PIXEL_FORMAT_GRBG_16,VC5_DECODER_PIXEL_FORMAT_GRBG_16,16,{{1,0,3,2}}},
    {VC5_ENCODER_PIXEL_FORMAT_BGGR_12,VC5_DECODER_PIXEL_FORMAT_BGGR_12,12,{{3,1,2,0}}},
    {VC5_ENCODER_PIXEL_FORMAT_BGGR_14,VC5_DECODER_PIXEL_FORMAT_BGGR_14,14,{{3,1,2,0}}},
    {VC5_ENCODER_PIXEL_FORMAT_BGGR_16,VC5_DECODER_PIXEL_FORMAT_BGGR_16,16,{{3,1,2,0}}}
};
static std::vector<uint8_t> encode(const std::vector<uint8_t> &input,unsigned offset,
                                  unsigned pitch,unsigned width,unsigned height,const Format &f,int quality) {
    vc5_encoder_parameters p; vc5_encoder_parameters_set_default(&p);
    p.input_width=width; p.input_height=height; p.input_pitch=pitch; p.pixel_format=f.encoder;
    p.quality_setting=static_cast<VC5_ENCODER_QUALITY_SETTING>(quality);
    gpr_buffer src={const_cast<uint8_t *>(input.data()+offset),input.size()-offset},out={};
    check(!vc5_encoder_process(&p,&src,&out,nullptr),"encode failed");
    std::unique_ptr<void,decltype(&std::free)> owner(out.buffer,std::free);
    auto *bytes=static_cast<const uint8_t *>(out.buffer);
    return std::vector<uint8_t>(bytes,bytes+out.size);
}
static std::vector<uint16_t> decode(const std::vector<uint8_t> &input,VC5_DECODER_PIXEL_FORMAT format) {
    vc5_decoder_parameters p; vc5_decoder_parameters_set_default(&p); p.pixel_format=format;
    p.mem_alloc=std::malloc; p.mem_free=std::free;
    p.rgb_resolution=GPR_RGB_RESOLUTION_NONE;
    gpr_buffer src={const_cast<uint8_t *>(input.data()),input.size()},out={};
    check(!vc5_decoder_process(&p,&src,&out,nullptr),"decode failed");
    std::unique_ptr<void,decltype(&std::free)> owner(out.buffer,std::free);
    auto *words=static_cast<const uint16_t *>(out.buffer);
    return std::vector<uint16_t>(words,words+out.size/2);
}
int main(int argc,char **argv) {
    try {
        check(argc==2 || (argc==3 && std::string(argv[2])=="--large"),"fixture directory [--large] required");
        bool large=argc==3;
        const unsigned height=large?3072:96;
        unsigned cases=0;
        for (unsigned width:(large?std::vector<unsigned>{4096}:std::vector<unsigned>{72,128})) {
          for (unsigned pattern=0;pattern<(large?1u:2u);++pattern) {
            // Existing stock fixture input; preserves independent historical byte evidence.
            std::vector<uint16_t> canonical(width*height);
            for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
                canonical[y*width+x]=(x*123+y*97+x*y*29+(x^y)*37)&16383;
            if (pattern) {
                // Include black, clipping, opposite green sites and sharp transitions.
                const uint16_t edges[]={0,16383,4,8192,16380,0,4096,12288};
                for (unsigned i=0;i<canonical.size();++i) canonical[i]=edges[i%8];
            }
            std::vector<uint8_t> reference_input(canonical.size()*2);
            std::memcpy(reference_input.data(),canonical.data(),reference_input.size());
            for (int quality=0;quality<6;++quality) {
                if (large && quality!=3) continue;
                auto reference=encode(reference_input,0,width*2,width,height,formats[1],quality);
                if (quality==3 && !pattern && !large) {
                    std::ifstream file(std::string(argv[1])+"/"+std::to_string(width)+"x96-q3.vc5",std::ios::binary);
                    std::vector<uint8_t> golden((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
                    check(!golden.empty() && reference==golden,"historical stock bytes changed");
                }
                auto linear16=decode(reference,VC5_DECODER_PIXEL_FORMAT_RGGB_16);
                check(linear16.size()==canonical.size(),"reference dimensions");
                for (const auto &f:formats) {
                    // New fallback additionally accepts byte-unaligned rows/padded pitches.
                    bool generic=f.encoder>=VC5_ENCODER_PIXEL_FORMAT_RGGB_16;
                    unsigned pitch=width*2+(generic?13:16),offset=generic?1:0;
                    std::vector<uint8_t> input(offset+pitch*height+16,0xDA);
                    for (unsigned y=0;y<height;y+=2) for (unsigned x=0;x<width;x+=2) {
                        for (unsigned c=0;c<4;++c) {
                            unsigned src=(y+c/2)*width+x+c%2,position=f.positions[c];
                            unsigned shift=f.bits-12;
                            uint16_t value=uint16_t(((canonical[src]>>2)<<shift) |
                                ((src*13+7)&((1u<<shift)-1)));
                            std::memcpy(input.data()+offset+(y+position/2)*pitch+(x+position%2)*2,&value,2);
                        }
                    }
                    const auto before=input;
                    auto compressed=encode(input,offset,pitch,width,height,f,quality);
                    check(compressed==reference,"Bayer format changed canonical stock payload");
                    check(input==before,"input or padding modified");
                    auto output=decode(compressed,f.decoder);
                    check(output.size()==canonical.size(),"output dimensions");
                    for (unsigned y=0;y<height;y+=2) for (unsigned x=0;x<width;x+=2)
                        for (unsigned c=0;c<4;++c) {
                            unsigned p=f.positions[c];
                            check(output[(y+p/2)*width+x+p%2]==
                                (linear16[(y+c/2)*width+x+c%2]>>(16-f.bits)),"decoded phase/domain mismatch");
                        }
                    ++cases;
                }
            }
          }
        }
        std::cout<<"PASS: "<<cases<<" Bayer/bit-depth/quality cases; "
                 <<(large?"4096x3072 stock-domain equivalence":"historical fixtures exact")<<'\n';
    } catch (const std::exception &e) { std::cerr<<e.what()<<'\n'; return 1; }
}
