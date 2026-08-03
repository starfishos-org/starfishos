/*
 * test_stale_tlb - does a munmap on one machine reach the other machines' TLBs?
 *
 * vmspace_unmap_range() (kernel/mm/vmregion.c) clears the unmapped PTEs in
 * every machine's page table, but the shootdown it issues afterwards --
 * flush_tlbs() -> flush_tlb_local_and_remote() -- only walks PLAT_CPU_NUM
 * entries of vmspace->history_cpus[], i.e. the CPUs of the machine running the
 * syscall.  vmspace->history_cpus[] is itself only PLAT_CPU_NUM bytes wide and
 * is indexed by the machine-local CPU id, so all machines alias the same slots
 * and no caller can distinguish them.  The cross-machine helper that does
 * exist, flush_tlb_on_remote_machine() (kernel/arch/x86_64/mm/tlb.c), is
 * declared in mm.h but never called.
 *
 * So after machine 0 unmaps a range, machine N's CPUs should keep translating
 * it until those entries are evicted naturally.  This test asks directly:
 *
 *   1. machine 0 maps pages and writes PATTERN
 *   2. a thread pinned to a CPU on the remote machine reads them, which loads
 *      the translations into that machine's TLB
 *   3. machine 0 munmaps every page
 *   4. the remote thread reads the same addresses again
 *
 * Step 4 has two possible outcomes, and both are informative:
 *
 *   - it still returns PATTERN  -> the remote CPU answered from a translation
 *     that outlived the mapping.  Printed as RESULT: STALE.
 *   - it faults                 -> the shootdown did reach this machine (or
 *     the entry was evicted).  The kernel prints "no vmr found for va <addr>"
 *     and kills the process; the "[stale-tlb] step 4" line below is the last
 *     thing printed, so that death is the expected clean outcome, not a
 *     mystery crash.
 *
 * Note the addresses are never reused: vmspace_mmap_with_pmo() bumps
 * user_current_mmap_addr and never recycles ("TODO: for simplicity, just keep
 * increasing the mmap_addr now"), so a stale entry here points at a page that
 * is no longer reachable by any name rather than at some later object.
 *
 * Usage: test_stale_tlb.bin [remote_global_cpu] [pages]
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SIZE   4096
#define MAX_PAGES   64
#define PATTERN     0xA5A5A5A5A5A5A5A5ULL

/* Control block: mapped once and never unmapped, so it stays translatable. */
struct ctrl {
    volatile unsigned long phase;      /* 1 = prime, 2 = primed, 3 = unmapped */
    volatile unsigned long observed[MAX_PAGES];
    volatile unsigned long remote_cpu;
    volatile unsigned long ready;
    volatile unsigned long npages;
    void *volatile addr[MAX_PAGES];
};

static struct ctrl *ctrl;

static void wait_phase(unsigned long want)
{
    while (ctrl->phase < want)
        __asm__ volatile("pause" ::: "memory");
}

static void *remote_thread(void *arg)
{
    cpu_set_t set;
    unsigned long i;

    (void)arg;

    CPU_ZERO(&set);
    CPU_SET(ctrl->remote_cpu, &set);
    /* -2: set the *global* affinity of the calling thread, which migrates it
     * to the machine owning that CPU (see sys_set_affinity in
     * kernel/object/thread.c). */
    if (sched_setaffinity(-2, sizeof(set), &set) != 0) {
        printf("[stale-tlb] FAILED: cannot bind to global cpu %lu\n",
               ctrl->remote_cpu);
        ctrl->ready = 2;
        return NULL;
    }
    sched_yield();
    ctrl->ready = 1;

    /* Step 2: touch every page so this machine caches the translations. */
    wait_phase(1);
    for (i = 0; i < ctrl->npages; i++)
        ctrl->observed[i] = *(volatile unsigned long *)ctrl->addr[i];
    ctrl->phase = 2;

    /* Step 4: same addresses, now unmapped. */
    wait_phase(3);
    for (i = 0; i < ctrl->npages; i++)
        ctrl->observed[i] = *(volatile unsigned long *)ctrl->addr[i];
    ctrl->phase = 4;

    return NULL;
}

int main(int argc, char *argv[])
{
    unsigned long remote_cpu = 36;   /* machine 3, cpu 0, on a 12-vCPU guest */
    unsigned long npages = 8;
    unsigned long i, stale = 0, primed_ok = 0;
    pthread_t tid;
    void *p;

    if (argc > 1)
        remote_cpu = strtoul(argv[1], NULL, 0);
    if (argc > 2)
        npages = strtoul(argv[2], NULL, 0);
    if (npages > MAX_PAGES)
        npages = MAX_PAGES;

    ctrl = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ctrl == MAP_FAILED) {
        printf("[stale-tlb] FAILED: cannot map control page\n");
        return 1;
    }
    memset((void *)ctrl, 0, PAGE_SIZE);
    ctrl->remote_cpu = remote_cpu;
    ctrl->npages = npages;

    printf("[stale-tlb] remote global cpu %lu, %lu page(s)\n",
           remote_cpu, npages);

    /* Step 1: map and fill. */
    for (i = 0; i < npages; i++) {
        p = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            printf("[stale-tlb] FAILED: mmap %lu\n", i);
            return 1;
        }
        ctrl->addr[i] = p;
        *(volatile unsigned long *)p = PATTERN;
    }

    if (pthread_create(&tid, NULL, remote_thread, NULL) != 0) {
        printf("[stale-tlb] FAILED: pthread_create\n");
        return 1;
    }
    while (ctrl->ready == 0)
        sched_yield();
    if (ctrl->ready == 2)
        return 1;

    ctrl->phase = 1;
    wait_phase(2);

    for (i = 0; i < npages; i++) {
        if (ctrl->observed[i] == PATTERN)
            primed_ok++;
    }
    printf("[stale-tlb] primed: remote machine read the pattern on %lu/%lu "
           "page(s) at %p..\n", primed_ok, npages, ctrl->addr[0]);

    /* Step 3: drop every mapping.  Only machine 0's TLBs are shot down. */
    for (i = 0; i < npages; i++)
        munmap(ctrl->addr[i], PAGE_SIZE);

    printf("[stale-tlb] step 4: remote machine re-reads the unmapped pages; "
           "a fault here (no vmr found) is the CORRECT outcome\n");
    fflush(stdout);

    ctrl->phase = 3;
    wait_phase(4);
    pthread_join(tid, NULL);

    for (i = 0; i < npages; i++) {
        if (ctrl->observed[i] == PATTERN) {
            stale++;
            if (stale <= 4)
                printf("[stale-tlb] page %lu at %p: remote still reads 0x%llx "
                       "after munmap\n", i, ctrl->addr[i], PATTERN);
        }
    }

    if (stale) {
        printf("[stale-tlb] RESULT: STALE - %lu/%lu unmapped page(s) are still "
               "translated on the remote machine\n", stale, npages);
    } else {
        printf("[stale-tlb] RESULT: OK - no unmapped page was still readable "
               "from the remote machine\n");
    }
    printf("[stale-tlb] done\n");
    return stale ? 1 : 0;
}
