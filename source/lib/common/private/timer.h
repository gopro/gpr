/*! @file gpr_timer.h
 *
 *  @brief Declaration of a high-resolution performance timer.
 *
 *  Measures elapsed wall-clock time from a monotonic source. This is not CPU
 *  time: clock()-style accounting sums processor time across every thread of
 *  the process, so multithreaded runs report larger numbers the more threads
 *  they use, even as the user waits less.
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

#ifndef GPR_TIMER_H
#define GPR_TIMER_H

#include <stdint.h>

typedef struct timer
{
    int64_t begin;      //!< Timestamp at StartTimer, in nanoseconds
    int64_t elapsed;    //!< Accumulated elapsed wall-clock time, in nanoseconds

} TIMER;

#ifdef __cplusplus
extern "C" {
#endif

    void InitTimer(TIMER *timer);

    void StartTimer(TIMER *timer);

    void StopTimer(TIMER *timer);

    float TimeSecs(TIMER *timer);

    float TimeMSecs(TIMER *timer);

#ifdef __cplusplus
}
#endif

#endif // GPR_TIMER_H
