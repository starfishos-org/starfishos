#include <common/lock.h>
#include <common/kprint.h>
#include <arch/machine/smp.h>
#include <common/macro.h>
#include <mm/kmalloc.h>

#include "tests.h"

static volatile u32 test_round_arrived;
static volatile u32 test_round_phase;

static void barrier_all_cpus(u32 online_cpu_num)
{
    u32 phase;

    lock(&big_kernel_lock);
    phase = test_round_phase;
    test_round_arrived++;
    if (test_round_arrived == online_cpu_num) {
        test_round_arrived = 0;
        test_round_phase++;
    }
    unlock(&big_kernel_lock);

    while (test_round_phase == phase)
        ;
}

void run_test(void)
{
    // tst_mutex();
    // tst_rwlock();
    u32 cpu_id = smp_get_cpu_id();
    u32 online_cpu_num = smp_get_cpu_num();
    static const u32 parallel_levels[] = {1, 4, 16, 32, 48, 64, 96};

    if (online_cpu_num == 0)
        online_cpu_num = PLAT_CPU_NUM;

    for (u32 i = 0; i < sizeof(parallel_levels) / sizeof(parallel_levels[0]); i++) {
        u32 parallel_num = parallel_levels[i];

        if (parallel_num > online_cpu_num) {
            if (cpu_id == 0) {
                kinfo("[TEST] skip malloc test parallel=%u (online cpus=%u)\n",
                      parallel_num,
                      online_cpu_num);
            }
            barrier_all_cpus(online_cpu_num);
            continue;
        }

        if (cpu_id == 0) {
            kinfo("[TEST] start malloc test parallel=%u\n", parallel_num);
        }

        if (cpu_id < parallel_num) {
            tst_malloc(parallel_num);
        }

        barrier_all_cpus(online_cpu_num);
    }
    // tst_sched();
    // tst_malloc_latency(1);
}
