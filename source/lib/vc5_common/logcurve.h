/*! @file logcurve.h
 *
 *  @brief Declaration of the data structures and constants used to do log conversion.
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

#ifndef LOGCURVE_H
#define LOGCURVE_H

#define LOG_CURVE_TABLE_LENGTH      (1 << 12)
#define LOG_CURVE_TABLE_LENGTH_12   LOG_CURVE_TABLE_LENGTH
#define LOG_CURVE_TABLE_LENGTH_14   (1 << 14)
#define LOG_CURVE_TABLE_LENGTH_16   (1 << 16)

#ifdef __cplusplus
extern "C" {
#endif

int vc5_logcurve_bypass(void);
    
#define EncoderLogCurve EncoderLogCurve12
extern uint16_t EncoderLogCurve12[];
extern uint16_t EncoderLogCurve14[];
extern uint16_t EncoderLogCurve16[];

extern uint16_t DecoderLogCurve12[];
extern uint16_t DecoderLogCurve14[];
extern uint16_t DecoderLogCurve16[];

static INLINE uint16_t DecodeLogValue(uint32_t value, int bits)
{
    if (vc5_logcurve_bypass())
    {
        const uint32_t max_bits = (bits >= 16) ? 16 : (bits <= 0 ? 1 : bits);
        const uint32_t max_val = (1u << max_bits) - 1;
        if (value > max_val) value = max_val;
        return (uint16_t)value;
    }

    const uint32_t max12 = (1u << 12) - 1;
    const uint32_t max14 = (1u << 14) - 1;
    const uint32_t max16 = (1u << 16) - 1;

    if (bits <= 12)
    {
        if (value > max12) value = max12;
        return DecoderLogCurve12[value];
    }
    else if (bits <= 14)
    {
        if (value > max14) value = max14;
        return DecoderLogCurve14[value];
    }
    else
    {
        if (value > max16) value = max16;
        return DecoderLogCurve16[value];
    }
}

	void SetupDecoderLogCurve(void);

void SetupEncoderLogCurve(void);

#ifdef __cplusplus
}
#endif

#endif // LOGCURVE_H
