#pragma once
#include <common/macro.h>
#include <irq/timer.h>

#define DECLTMR                         \
        unsigned long long timer_start; \
        long long total_time = 0;       \
        UNUSED(total_time);             \
        UNUSED(timer_start);

#define start() (timer_start = plat_get_mono_time())
#define stop() (long)(total_time += plat_get_mono_time() - timer_start)

/* allow double time tracker */
#define DECLTMR2                         \
        unsigned long long timer_start2 = 0; \
        long long total_time2 = 0;       \
        UNUSED(total_time2);             \
        UNUSED(timer_start2);

#define start2() (timer_start2 = plat_get_mono_time())
#define stop2() (long)(total_time2 += plat_get_mono_time() - timer_start2)
