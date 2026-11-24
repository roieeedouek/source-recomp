// mach_absolute_time on Linux: nanosecond monotonic clock, timebase 1/1.
#pragma once
#include <stdint.h>
#include <time.h>
typedef struct mach_timebase_info { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = t->denom = 1; return 0; }
static inline uint64_t mach_absolute_time(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
