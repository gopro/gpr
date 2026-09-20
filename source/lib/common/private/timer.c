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

// glibc hides clock_gettime, CLOCK_MONOTONIC, and struct timespec in strict
// ISO C mode (-std=c99) unless _POSIX_C_SOURCE >= 199309L. Must be defined
// before the first system header is included.
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 199309L
#endif

#include "timer.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/*!
	@brief Current monotonic wall-clock time in nanoseconds, relative to the
	first call.

	Wall time from a monotonic clock, deliberately not clock(): that sums CPU
	time over every thread of the process, so a multithreaded run measures
	larger, not smaller, as threads are added.

	The value is relative to a base captured by TimerCaptureBase() when the
	program loads, so a zero-initialized TIMER that is stopped without ever
	being started (such as the global log timer) reads as elapsed time since
	program start, as the clock()-based implementation did -- including when
	the first timer use is a single log line at the very end of the run.
 */
static int64_t MonotonicNSecs(void)
{
	static int64_t base = 0;
	int64_t now;

#if defined(_WIN32)
	static LARGE_INTEGER frequency = { 0 };
	LARGE_INTEGER counter;

	if (frequency.QuadPart == 0)
		QueryPerformanceFrequency(&frequency);

	QueryPerformanceCounter(&counter);
	now = (int64_t)((double)counter.QuadPart * 1000000000.0 / (double)frequency.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif

	if (base == 0)
		base = now;

	return now - base;
}

// Capture the monotonic base before main() runs. Without this the epoch would be
// anchored wherever the first timer call happens to be, and a run whose only log
// line is at the end would measure its whole duration as zero.
#if defined(_MSC_VER)
static int TimerCaptureBase(void) { MonotonicNSecs(); return 0; }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static int (*timer_capture_base_)(void) = TimerCaptureBase;
#else
__attribute__((constructor)) static void TimerCaptureBase(void)
{
	MonotonicNSecs();
}
#endif

void InitTimer(TIMER *timer)
{
    timer->begin    = 0;
	timer->elapsed  = 0;
}

void StartTimer(TIMER *timer)
{
    timer->begin    = MonotonicNSecs();
}

void StopTimer(TIMER *timer)
{
	timer->elapsed += (MonotonicNSecs() - timer->begin);
}

float TimeSecs(TIMER *timer)
{
	return (float)(timer->elapsed) / 1000000000.0f;
}

float TimeMSecs(TIMER *timer)
{
    return (float)(TimeSecs(timer) * 1000);
}

