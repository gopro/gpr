/* SPDX-License-Identifier: Apache-2.0 OR MIT */
#ifndef GPR_PREVIEW_DEQUANTIZE_H
#define GPR_PREVIEW_DEQUANTIZE_H
#include <stdint.h>
/* Private streaming-preview helper. The immutable lookup combines the stock
 * cubic codebook map with UncompandedValue; entries are in 0..1023. Caller
 * validates quantization in 1..32767. The product therefore fits int32_t.
 * Preserve the asymmetric signed-16 saturation of DequantizeBandRow16s. */
static inline int16_t gpr_preview_dequantize_sample(const int16_t *lookup,
    int16_t coefficient,uint32_t quantization)
{
    int32_t value=coefficient;
    uint32_t magnitude=(uint32_t)(value<0?-value:value);
    if(magnitude>1023)magnitude=1023;
    int32_t scaled=(int32_t)lookup[magnitude]*(int32_t)quantization;
    if(value<0)return (int16_t)(scaled>32768?-32768:-scaled);
    return (int16_t)(scaled>32767?32767:scaled);
}
/* The signed table contains exact stock-dequantized values for -1023..1023. */
static inline int16_t gpr_preview_quantized_lookup_sample(const int16_t *lookup,
    int16_t coefficient)
{
    int32_t value=coefficient;
    if(value < -1023)value=-1023;
    if(value > 1023)value=1023;
    return lookup[value+1023];
}
#endif
