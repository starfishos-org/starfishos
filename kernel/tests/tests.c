#include <common/lock.h>
#include <common/kprint.h>
#include <arch/machine/smp.h>
#include <common/macro.h>
#include <mm/kmalloc.h>

#include "tests.h"

void run_test(void)
{
    // tst_mutex();
    // tst_rwlock();
    u32 cpu_id = smp_get_cpu_id();
    u32 parallel_num = 1;
    if (cpu_id < parallel_num) {
        tst_malloc(parallel_num);
    }
    // tst_sched();
    // tst_malloc_latency(1);
}
