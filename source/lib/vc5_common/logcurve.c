/*! @file logcurve.c
 *
 *  @brief Implementation of functions used to do log conversion.
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

#include "common.h"

int vc5_logcurve_bypass(void)
{
    return 0;
}

uint16_t EncoderLogCurve12[LOG_CURVE_TABLE_LENGTH_12];
uint16_t EncoderLogCurve14[LOG_CURVE_TABLE_LENGTH_14];
uint16_t EncoderLogCurve16[LOG_CURVE_TABLE_LENGTH_16];

uint16_t DecoderLogCurve12[LOG_CURVE_TABLE_LENGTH_12];
uint16_t DecoderLogCurve14[LOG_CURVE_TABLE_LENGTH_14];
uint16_t DecoderLogCurve16[LOG_CURVE_TABLE_LENGTH_16];

static void SetupDecoderCurve(uint16_t *table, int bits)
{
    const int max_input = (1 << bits) - 1;
    const double denom = log10(113.0);
    const int max_output = (1 << 16) - 1;

    // Antilog: the mathematical inverse of the encoder log curve.
    // Encoder: y = max_out * log10(x/max_in * 112 + 1) / log10(113)
    // Decoder: x = max_out * (10^(y/max_in * log10(113)) - 1) / 112
    for (int i = 0; i <= max_input; ++i)
    {
        const double norm = (double)i / max_input;
        const double exp_val = pow(10.0, norm * denom);
        const double output = max_output * (exp_val - 1.0) / 112.0;
        int out_int = (int)(output + 0.5);
        if (out_int < 0) out_int = 0;
        if (out_int > max_output) out_int = max_output;
        table[i] = (uint16_t)out_int;
    }
}

void SetupDecoderLogCurve(void)
{
    SetupDecoderCurve(DecoderLogCurve12, 12);
    SetupDecoderCurve(DecoderLogCurve14, 14);
    SetupDecoderCurve(DecoderLogCurve16, 16);
}

static void SetupEncoderLogCurveBits(uint16_t* table, int input_bits, int output_bits)
{
    const int max_input_val = (1 << input_bits) - 1;
    const int max_output_val = (1 << output_bits) - 1;
    const double denom = log10(113.0);

    for(int i = 0; i <= max_input_val; i++ )
    {
        const double input = maximum(0, i);
        const double norm  = (input / max_input_val * 112.0) + 1.0;
        const double output = max_output_val * (log10(norm) / denom);
        table[i] = (uint16_t)output;
    }
}

void SetupEncoderLogCurve(void)
{
    SetupEncoderLogCurveBits(EncoderLogCurve12, 12, 12);
    SetupEncoderLogCurveBits(EncoderLogCurve14, 14, 14);
    SetupEncoderLogCurveBits(EncoderLogCurve16, 16, 16);
}
