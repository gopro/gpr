/*! @file gpr_timer.c
 *
 *  @brief Implementation of a high-resolution performance timer.
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

#include "timer.h"

/*!
	@brief Initialize a timer
	
	The frequency of the performance timer is determined if it has not
	already been obtained.
 */
void InitTimer(TIMER *timer)
{
    timer->begin    = 0;
	timer->elapsed  = 0;
}

void StartTimer(TIMER *timer)
{
    timer->begin    = clock();
}

void StopTimer(TIMER *timer)
{
	timer->elapsed += (clock() - timer->begin);
}

float TimeSecs(TIMER *timer)
{
	return (float)(timer->elapsed) / CLOCKS_PER_SEC;
}

float TimeMSecs(TIMER *timer)
{
    return (float)(TimeSecs(timer) * 1000);
}

