/*
 * Exercise the complete DRAM -> CXL -> DRAM page lifecycle.
 *
 * The main thread first faults a private anonymous range on machine 0 and
 * fills every word with a deterministic pattern. A worker then migrates to a
 * CPU on machine 1, verifies the original contents, and overwrites the range.
 * The worker migrates enough data to cross the configured high watermark and
 * force older CXL-resident pages back into their origin DRAM pages. Finally,
 * the main thread verifies the worker's pattern from machine 0. The test also
 * checks the kernel's reclaimed-page counter, so data verification alone
 * cannot produce a false PASS without exercising CXL-to-DRAM migration.
 *
 * Usage: test_cxl_reclaim.bin [MiB] [remote_global_cpu]
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chcore/syscall.h>

#ifndef MAP_FLAG_PRIVATE
#define MAP_FLAG_PRIVATE 0x400000
#endif
#define DEFAULT_MIB 1000UL
#define DEFAULT_REMOTE_CPU 12UL
#define WRITE_XOR 0xd1b54a32d192ed03ULL

struct test_state {
    uint64_t *base;
    size_t pages;
    unsigned long remote_cpu;
    unsigned long read_errors;
    uint64_t reclaimed_before;
    uint64_t reclaimed_after;
    int affinity_error;
};

static uint64_t initial_value(size_t page, size_t word)
{
    return 0x9e3779b97f4a7c15ULL * (page + 1)
           ^ 0xbf58476d1ce4e5b9ULL * (word + 1);
}

static void *remote_worker(void *opaque)
{
    struct test_state *state = opaque;
    const size_t words_per_page = PAGE_SIZE / sizeof(uint64_t);
    cpu_set_t set;
    size_t page;
    size_t word;

    CPU_ZERO(&set);
    CPU_SET(state->remote_cpu, &set);
    if (sched_setaffinity(-2, sizeof(set), &set) != 0) {
        state->affinity_error = 1;
        return NULL;
    }
    sched_yield();

    printf("[cxl-reclaim-test] remote phase on global cpu %lu\n",
           state->remote_cpu);
    state->reclaimed_before = usys_get_cxl_reclaimed_pages();
    for (page = 0; page < state->pages; page++) {
        uint64_t *page_base = state->base + page * words_per_page;

        for (word = 0; word < words_per_page; word++) {
            uint64_t expected = initial_value(page, word);

            if (page_base[word] != expected)
                state->read_errors++;
            page_base[word] = expected ^ WRITE_XOR;
        }
    }
    state->reclaimed_after = usys_get_cxl_reclaimed_pages();
    return NULL;
}

int main(int argc, char **argv)
{
    const size_t words_per_page = PAGE_SIZE / sizeof(uint64_t);
    unsigned long mib = DEFAULT_MIB;
    struct test_state state = {0};
    unsigned long final_errors = 0;
    size_t bytes;
    size_t page;
    size_t word;
    pthread_t worker;
    cpu_set_t local_set;

    if (argc > 1)
        mib = strtoul(argv[1], NULL, 0);
    state.remote_cpu = argc > 2 ? strtoul(argv[2], NULL, 0)
                                : DEFAULT_REMOTE_CPU;
    if (mib == 0)
        mib = DEFAULT_MIB;

    bytes = (size_t)mib * 1024 * 1024;
    state.pages = bytes / PAGE_SIZE;
    state.base = mmap(NULL,
                      state.pages * PAGE_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FLAG_PRIVATE,
                      -1,
                      0);
    if (state.base == MAP_FAILED) {
        printf("[cxl-reclaim-test] RESULT: FAIL mmap\n");
        return 1;
    }

    printf("[cxl-reclaim-test] initializing %lu MiB (%lu pages) on machine 0\n",
           mib,
           (unsigned long)state.pages);
    for (page = 0; page < state.pages; page++) {
        uint64_t *page_base = state.base + page * words_per_page;

        for (word = 0; word < words_per_page; word++)
            page_base[word] = initial_value(page, word);
    }

    if (pthread_create(&worker, NULL, remote_worker, &state) != 0) {
        printf("[cxl-reclaim-test] RESULT: FAIL pthread_create\n");
        return 1;
    }
    pthread_join(worker, NULL);
    if (state.affinity_error) {
        printf("[cxl-reclaim-test] RESULT: FAIL affinity\n");
        return 1;
    }

    CPU_ZERO(&local_set);
    CPU_SET(0, &local_set);
    if (sched_setaffinity(-2, sizeof(local_set), &local_set) != 0) {
        printf("[cxl-reclaim-test] RESULT: FAIL return affinity\n");
        return 1;
    }
    sched_yield();

    printf("[cxl-reclaim-test] verifying returned data on machine 0\n");
    for (page = 0; page < state.pages; page++) {
        uint64_t *page_base = state.base + page * words_per_page;

        for (word = 0; word < words_per_page; word++) {
            uint64_t expected = initial_value(page, word) ^ WRITE_XOR;

            if (page_base[word] != expected)
                final_errors++;
        }
    }

    munmap(state.base, state.pages * PAGE_SIZE);
    printf("[cxl-reclaim-test] remote_errors=%lu final_errors=%lu "
           "reclaimed_delta=%lu\n",
           state.read_errors,
           final_errors,
           (unsigned long)(state.reclaimed_after - state.reclaimed_before));
    if (state.read_errors || final_errors) {
        printf("[cxl-reclaim-test] RESULT: FAIL data mismatch\n");
        return 1;
    }
    if (state.reclaimed_after <= state.reclaimed_before) {
        printf("[cxl-reclaim-test] RESULT: FAIL no CXL page was reclaimed\n");
        return 1;
    }
    printf("[cxl-reclaim-test] RESULT: PASS\n");
    return 0;
}
