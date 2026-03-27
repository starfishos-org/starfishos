#include "common/types.h"
#include <common/lock.h>
#include <common/kprint.h>
#include <arch/machine/smp.h>
#include <common/macro.h>
#include <mm/kmalloc.h>
#include <mm/buddy.h>
#include <irq/timer.h>

#include "tests.h"

#include <arch/machine/pmu.h>

#define MALLOC_TEST_NUM   512
#define MALLOC_TEST_ROUND 10

/*
 * get_pages benchmark: draining an entire CXL pool can take millions of
 * iterations and look like a hang with no log output. Cap by default; set to
 * 0 to drain until OOM (legacy behavior).
 */
#ifndef GET_PAGES_BENCHMARK_MAX_CHUNKS
#define GET_PAGES_BENCHMARK_MAX_CHUNKS 50000u
#endif

volatile int malloc_start_flag = 0;
volatile int malloc_finish_flag = 0;
volatile int malloc_reset_phase = 0;
volatile int malloc_reset_arrived = 0;
static u64 kmalloc_tp_percpu[PLAT_CPU_NUM];
extern int malloc_data[];

static void reset_malloc_barriers(u32 parallel_num)
{
    int phase;

    while (malloc_finish_flag != parallel_num)
        ;

    lock(&big_kernel_lock);
    phase = malloc_reset_phase;
    malloc_reset_arrived++;
    if (malloc_reset_arrived == (int)parallel_num) {
            malloc_reset_arrived = 0;
            malloc_start_flag = 0;
            malloc_finish_flag = 0;
            malloc_reset_phase++;
    }
    unlock(&big_kernel_lock);

    while (malloc_reset_phase == phase)
        ;
}

static void run_kmalloc_throughput(u32 parallel_num, const char *tag, mem_t flags)
{
    char **buf;
    unsigned long start, end;
    int total_malloc_num = MALLOC_TEST_NUM * MALLOC_TEST_ROUND;
    u32 cpu_id = smp_get_cpu_id();

    buf = (char **)kmalloc(sizeof(char *) * MALLOC_TEST_NUM, flags);

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
            buf[i] = kmalloc(size, flags);
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
    kmalloc_tp_percpu[cpu_id] = (u64)(total_malloc_num
                                      / ((double)(end - start) / 1000000000));
    malloc_finish_flag++;
    unlock(&big_kernel_lock);
    while (malloc_finish_flag != parallel_num)
        ;

    if (cpu_id == 0) {
        u64 sum = 0;
        for (u32 i = 0; i < parallel_num; i++)
            sum += kmalloc_tp_percpu[i];
        kinfo("[TEST] %s kmalloc avg throughput (parallel=%u): %llu ops/s\n",
              tag,
              parallel_num,
              sum / parallel_num);
    }
    /* ============ Finish Barrier ============ */

    kfree((void *)buf);
}

typedef struct page_list_node {
    struct page_list_node *next;
} page_list_node_t;

static void run_get_pages_full_once(u32 parallel_num,
                                    const char *tag,
                                    mem_t flags,
                                    int order,
                                    unsigned long size_bytes)
{
    void *addr;
    page_list_node_t *head = NULL;
    page_list_node_t *node;
    u64 alloc_num = 0;
    unsigned long alloc_start, alloc_end;
    unsigned long free_start, free_end;
    u64 alloc_tp = 0;
    u64 free_tp = 0;

    lock(&big_kernel_lock);
    malloc_start_flag++;
    unlock(&big_kernel_lock);
    while (malloc_start_flag != parallel_num)
        ;

    if (smp_get_cpu_id() == 0) {
        alloc_start = pmu_read_real_cycle();
        while (1) {
            if (GET_PAGES_BENCHMARK_MAX_CHUNKS != 0
                && alloc_num >= GET_PAGES_BENCHMARK_MAX_CHUNKS)
                break;
            addr = get_pages(order, flags);
            if (!addr)
                break;
            node = (page_list_node_t *)addr;
            node->next = head;
            head = node;
            alloc_num++;
        }
        alloc_end = pmu_read_real_cycle();

        free_start = pmu_read_real_cycle();
        while (head) {
            node = head;
            head = head->next;
            free_pages((void *)node);
        }
        free_end = pmu_read_real_cycle();

        kinfo("[TEST] get_pages alloc (%s, %luB): %llu chunks%s\n",
              tag,
              size_bytes,
              alloc_num,
              (GET_PAGES_BENCHMARK_MAX_CHUNKS != 0
               && alloc_num == GET_PAGES_BENCHMARK_MAX_CHUNKS)
                      ? " (capped)"
                      : "");
        if (alloc_num && likely(alloc_end > alloc_start)) {
            alloc_tp = (u64)(alloc_num
                             / ((double)(alloc_end - alloc_start) / 1000000000.0));
            kinfo("[TEST] get_pages throughput (%s, %luB): %llu ops/s (avg)\n",
                  tag,
                  size_bytes,
                  alloc_tp);
        }
        if (alloc_num && likely(free_end > free_start)) {
            free_tp = (u64)(alloc_num / ((double)(free_end - free_start) / 1000000000.0));
            kinfo("[TEST] free_pages throughput (%s, %luB): %llu ops/s (avg)\n",
                  tag,
                  size_bytes,
                  free_tp);
        }
        BUG_ON(alloc_num == 0);
    }

    lock(&big_kernel_lock);
    malloc_finish_flag++;
    unlock(&big_kernel_lock);
    while (malloc_finish_flag != parallel_num)
        ;
}

void tst_malloc(u32 parallel_num)
{
    run_kmalloc_throughput(parallel_num, "DRAM", __MT_PRIVATE__);
    reset_malloc_barriers(parallel_num);
    run_kmalloc_throughput(parallel_num, "CXL", __MT_SHARED__);
    reset_malloc_barriers(parallel_num);
    run_get_pages_full_once(parallel_num, "DRAM", __MT_PRIVATE__, 0, BUDDY_PAGE_SIZE);
    reset_malloc_barriers(parallel_num);
    run_get_pages_full_once(parallel_num, "DRAM", __MT_PRIVATE__, 9, (1UL << 9) * BUDDY_PAGE_SIZE);
    reset_malloc_barriers(parallel_num);
    run_get_pages_full_once(parallel_num, "CXL", __MT_SHARED__, 0, BUDDY_PAGE_SIZE);
    reset_malloc_barriers(parallel_num);
    run_get_pages_full_once(parallel_num, "CXL", __MT_SHARED__, 9, (1UL << 9) * BUDDY_PAGE_SIZE);
    reset_malloc_barriers(parallel_num);

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
