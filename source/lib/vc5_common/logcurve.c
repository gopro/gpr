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
#include "logcurve_tables.h"

uint16_t EncoderLogCurve[LOG_CURVE_TABLE_LENGTH];

uint16_t DecoderLogCurve[LOG_CURVE_TABLE_LENGTH];

void SetupDecoderLogCurve(void)
{
    /* Use precomputed table for cross-platform consistency.
       Runtime pow()/log10() produce different results on MSVC vs GCC/Clang,
       causing different wavelet coefficients and file sizes. */
    memcpy(DecoderLogCurve, DecoderLogCurve_static, sizeof(DecoderLogCurve));
}

void SetupEncoderLogCurve(void)
{
    /* Use precomputed table for cross-platform consistency. */
    memcpy(EncoderLogCurve, EncoderLogCurve_static, sizeof(EncoderLogCurve));
}

