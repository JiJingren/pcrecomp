/*
 * hle_ios_compat.c - hle_clock_gettime() for a Darwin SDK older than iOS 10,
 * which is what hle_ios_compat.h routes clock_gettime() to there. See that
 * header for why the real one cannot be used.
 */
#include "hle_ios_compat.h"

#if defined(HLE_IOS_COMPAT_CLOCK)

#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

static mach_timebase_info_data_t hle_tb;
static void hle_tb_load(void) { (void)mach_timebase_info(&hle_tb); }

int hle_clock_gettime(int clock_id, struct timespec *ts) {
    if (clock_id != CLOCK_MONOTONIC) {
        /* The wall clock needs nothing from the tick counter: gettimeofday has
         * always been there and is not what iOS 10 gated. */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts->tv_sec  = tv.tv_sec;
        ts->tv_nsec = (long)tv.tv_usec * 1000;
        return 0;
    }
    /* mach_absolute_time counts ticks at a frequency only the kernel reports,
     * and has done since long before iOS 6. */
    static pthread_once_t tb_once = PTHREAD_ONCE_INIT;
    pthread_once(&tb_once, hle_tb_load);
    uint64_t ns = (uint64_t)mach_absolute_time() * (uint64_t)hle_tb.numer / (uint64_t)hle_tb.denom;
    ts->tv_sec  = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

#endif