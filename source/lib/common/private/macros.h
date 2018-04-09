/*! @file gpr_macros.h
 *
 *  @brief Definitions of useful inline functions that are used everywhere in code.
 *
 *  @version 1.0.0
 *
 *  (C) Copyright 2018 GoPro Inc (http://gopro.com/).
 *
 *  Licensed under either:
 *  - Apache License, Version 2.0, http://www.apache.org/licenses/LICENSE-2.0
 *  - MIT license, http://opensource.org/licenses/MIT
 *  at your option.
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef GPR_MACROS_H
#define GPR_MACROS_H

#ifndef neg
    #define neg(x)	(-(x))
#endif

#define DivideByShift(x, s)		((x) >> (s))

STATIC_INLINE uint16_t clamp_uint(int32_t value, uint32_t precision)
{
    const int32_t limit = ((1 << precision) - 1);
    
    if (value < 0)
        value = 0;
    else if (value > limit)
        value = limit;
    
    return (uint16_t)value;
}

STATIC_INLINE uint16_t clamp_uint16(int32_t value)
{
    return (uint16_t)clamp_uint( value, 16);
}

STATIC_INLINE uint16_t clamp_uint14(int32_t value)
{
    return (uint16_t)clamp_uint( value, 14);
}

STATIC_INLINE uint16_t clamp_uint12(int32_t value)
{
    return (uint16_t)clamp_uint( value, 12);
}

STATIC_INLINE uint8_t clamp_uint8(int32_t value)
{
    return (uint8_t)clamp_uint( value, 8);
}

STATIC_INLINE int minimum(int a, int b)
{
    return (a < b) ? a : b;
}

STATIC_INLINE int maximum(int a, int b)
{
    return (a < b) ? b : a;
}

STATIC_INLINE int absolute(int a)
{
    return (a < 0) ? -a : a;
}

STATIC_INLINE uint32_t Swap32(uint32_t value)
{
    value = (value & 0x0000FFFF) << 16 | (value & 0xFFFF0000) >> 16;
    value = (value & 0x00FF00FF) << 8 | (value & 0xFF00FF00) >> 8;
    return value;
}

#endif // GPR_MACROS_H
