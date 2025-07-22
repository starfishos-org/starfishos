#include "common/types.h"
#include <common/lock.h>
#include <common/kprint.h>
#include <arch/machine/smp.h>
#include <common/macro.h>
#include <mm/kmalloc.h>
#include <irq/timer.h>

#include "tests.h"

#include <arch/machine/pmu.h>

#define MALLOC_TEST_NUM   512
#define MALLOC_TEST_ROUND 10

volatile int malloc_start_flag = 0;
volatile int malloc_finish_flag = 0;
extern int malloc_data[];

void tst_malloc(u32 parallel_num)
{
    char **buf;
    unsigned long start, end;
    int total_malloc_num = MALLOC_TEST_NUM * MALLOC_TEST_ROUND;
    u32 cpu_id = smp_get_cpu_id();

    buf = (char **)kmalloc(sizeof(char *) * MALLOC_TEST_NUM, __MT_DEFAULT__);

    /* ============ Start Barrier ============ */
    lock(&big_kernel_lock);
    malloc_start_flag++;
    unlock(&big_kernel_lock);
    while (malloc_start_flag != parallel_num)
        ;
    /* ============ Start Barrier ============ */

    start = pmu_read_real_cycle();

    for (int round = 0; round < MALLOC_TEST_ROUND; round++) {
        for (int i = 0; i < MALLOC_TEST_NUM; i++) {
            int size = 1 + malloc_data[((i + round * MALLOC_TEST_NUM) * cpu_id) % 95000];
            buf[i] = kmalloc(size, __MT_DEFAULT__);
            BUG_ON(!buf[i]);
            for (int j = 0; j < size; j++) {
                buf[i][j] = (char)(i + size);
            }
        }

        for (int i = 0; i < MALLOC_TEST_NUM; i++) {
            int size = 1 + malloc_data[((i + round * MALLOC_TEST_NUM) * cpu_id) % 95000];
            for (int j = 0; j < size; j++) {
                BUG_ON(buf[i][j] != (char)(i + size));
            }
            kfree(buf[i]);
        }
    }

    end = pmu_read_real_cycle();

    /* ============ Finish Barrier ============ */
    lock(&big_kernel_lock);
    // ops/s
    kinfo("CPU: %d, kmalloc test throughput: %ld\n",
          smp_get_cpu_id(),
          (u64)(total_malloc_num / ((double)(end - start) / 1000000000)));
    malloc_finish_flag++;
    unlock(&big_kernel_lock);
    while (malloc_finish_flag != parallel_num)
        ;
    /* ============ Finish Barrier ============ */

    kfree(buf);

    if (smp_get_cpu_id() == 0) {
        kinfo("[TEST] malloc succ!\n");
    }
}

#define ITERATIONS 5000
#define ALLOC_SIZE 4096

void tst_malloc_latency(bool one_cpu)
{
    unsigned long start, end;
    void *memory;
    int i;
    u64 sum_ns = 0;

    /* ============ Start Barrier ============ */
    lock(&big_kernel_lock);
    malloc_start_flag++;
    unlock(&big_kernel_lock);
    while (malloc_start_flag != PLAT_CPU_NUM)
        ;
    /* ============ Start Barrier ============ */

    if (smp_get_cpu_id() == 0) {
        kinfo("[TEST] start malloc test!\n");
    }

    if (one_cpu) {
        if (smp_get_cpu_id() != 0) {
            goto wait;
        }
    }

    for (i = 0; i < ITERATIONS; i++) {
        start = plat_get_mono_time();
        memory = kmalloc(ALLOC_SIZE, __MT_PRIVATE__);
        end = plat_get_mono_time();
        // kfree(memory);
        if (i % 1000 == 0) {
            kinfo("CPU: %d, DRAM kmalloc test takes %llu ns.\n",
                  smp_get_cpu_id(),
                  end - start);
        }
        sum_ns += (end - start);
    }

    (void)memory;

    kinfo("[TEST] CPU: %d, DRAM kmalloc test takes %llu ns.\n",
          smp_get_cpu_id(),
          sum_ns / ITERATIONS);

    sum_ns = 0;

    for (i = 0; i < ITERATIONS; i++) {
        start = plat_get_mono_time();
        memory = kmalloc(ALLOC_SIZE, __MT_SHARED__);
        end = plat_get_mono_time();
        // kfree(memory);
        if (i % 1000 == 0) {
            kinfo("CPU: %d, CXL kmalloc test takes %llu ns.\n",
                  smp_get_cpu_id(),
                  end - start);
        }
        sum_ns += (end - start);
    }

    kinfo("[TEST] CPU: %d, CXL kmalloc test takes %llu ns.\n",
          smp_get_cpu_id(),
          sum_ns / ITERATIONS);

wait:
    /* ============ Finish Barrier ============ */
    lock(&big_kernel_lock);
    malloc_finish_flag++;
    unlock(&big_kernel_lock);
    while (malloc_finish_flag != PLAT_CPU_NUM)
        ;
    /* ============ Finish Barrier ============ */

    if (smp_get_cpu_id() == 0) {
        kinfo("[TEST] malloc succ!\n");
    }
}