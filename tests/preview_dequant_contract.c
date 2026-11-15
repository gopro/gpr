/* Compare the streaming lookup with unchanged stock decoder arithmetic. */
#include "../source/lib/vc5_decoder/headers.h"
#include "../source/lib/gpr_video/gpr_preview_dequantize.h"
#include <stdio.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"preview dequant line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void)
{
    static PIXEL input[65536],expected[65536];
    int16_t cubic[1025],expanded[1025],signed_table[2047];
    const int quants[]={1,2,3,4,7,12,24,48,96,144,255,511,1023,2047,8192,16384,32767};
    REQUIRE(!ComputeCubicTable(cubic,1025,255));
    for(int i=0;i<1025;++i){
        int32_t value=UncompandedValue(cubic[i]);
        REQUIRE(value>=0&&value<=1023);expanded[i]=(int16_t)value;
    }
    for(int i=0;i<65536;++i){
        int value=i-32768,magnitude=value<0?-value:value;
        if(magnitude>1023)magnitude=1023;
        input[i]=(PIXEL)(value<0?-cubic[magnitude]:cubic[magnitude]);
    }
    unsigned checks=0;
    for(unsigned q=0;q<sizeof(quants)/sizeof(quants[0]);++q){
        REQUIRE(!DequantizeBandRow16s(input,65536,quants[q],expected));
        for(int value=-1023;value<=1023;++value)signed_table[value+1023]=gpr_preview_dequantize_sample(expanded,(int16_t)value,(uint32_t)quants[q]);
        for(int i=0;i<65536;++i){
            REQUIRE(gpr_preview_dequantize_sample(expanded,(int16_t)(i-32768),(uint32_t)quants[q])==expected[i]);
            REQUIRE(gpr_preview_quantized_lookup_sample(signed_table,(int16_t)(i-32768))==expected[i]);
            ++checks;
        }
    }
    printf("{\"coefficient_quantizer_comparisons\":%u,\"all_stock_dequantized_samples_exact\":true}\n",checks);
    return 0;
}
