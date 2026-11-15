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
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

uint16_t EncoderLogCurve[LOG_CURVE_TABLE_LENGTH];

uint16_t DecoderLogCurve[LOG_CURVE_TABLE_LENGTH];
uint8_t DecoderGamma8Curve[65536];

static void InitDecoderLogCurve(void)
{
    int i;
    const int log_table_size = sizeof(DecoderLogCurve) / sizeof(DecoderLogCurve[0]);
    
    const int max_16_bit = (1 << 16) - 1;
    
    for( i = 0; i < log_table_size; i++ )
    {
        //input 12-bit, output 16-bit
        float input = i;
        float output = max_16_bit * (pow(113.0, input/4095.0) - 1.0)/112.0;
        
        DecoderLogCurve[i]  = minimum( (int)output, max_16_bit );
    }
    /* Same integer result as the legacy per-pixel sqrtf/clamp, computed once. */
    for (i = 0; i < 65536; ++i) {
        int value = (int)sqrtf((float)i);
        DecoderGamma8Curve[i] = (uint8_t)minimum(value, 255);
    }
}

static void InitEncoderLogCurve(void)
{
    int i;
    const int max_input_val = LOG_CURVE_TABLE_LENGTH - 1;
    
    for( i = 0; i < LOG_CURVE_TABLE_LENGTH; i++ )
    {
        //input 16-bit, output 12-bit
        float input = maximum( 0, i );
        float output = 4095.0 * (log10(input/max_input_val * 112.0 + 1.0)/log10(113));

        EncoderLogCurve[i]  = ( (uint16_t)output );
    }
}

/* Preserve the historical arithmetic but initialize each shared table once.
 * Preview and still-thumbnail decoding can run alongside capture. */
#if defined(_WIN32)
static INIT_ONCE decoder_curve_once = INIT_ONCE_STATIC_INIT;
static INIT_ONCE encoder_curve_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK DecoderCurveOnce(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx; InitDecoderLogCurve(); return TRUE;
}
static BOOL CALLBACK EncoderCurveOnce(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx; InitEncoderLogCurve(); return TRUE;
}
void SetupDecoderLogCurve(void) { InitOnceExecuteOnce(&decoder_curve_once, DecoderCurveOnce, NULL, NULL); }
void SetupEncoderLogCurve(void) { InitOnceExecuteOnce(&encoder_curve_once, EncoderCurveOnce, NULL, NULL); }
#else
static pthread_once_t decoder_curve_once = PTHREAD_ONCE_INIT;
static pthread_once_t encoder_curve_once = PTHREAD_ONCE_INIT;
void SetupDecoderLogCurve(void) { pthread_once(&decoder_curve_once, InitDecoderLogCurve); }
void SetupEncoderLogCurve(void) { pthread_once(&encoder_curve_once, InitEncoderLogCurve); }
#endif

