/*
 * ChCore version of numa_bw_test: malloc DRAM and CXL memory, measure bandwidth.
 *
 * Uses mmap with MAP_FLAG_PRIVATE (DRAM) and MAP_FLAG_SHARED (CXL) to allocate
 * memory on different tiers, then sequential write to measure GB/s.
 *
 * Compile: ./quick-build.sh (with CHCORE_BUILD_SAMPLE_APPS_TESTS=ON)
 * Run: numa_bw_test.bin [size_bytes]
 *   size_bytes: optional, default 64MB. e.g. 0x4000000 = 64MB
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FLAG_SHARED
#define MAP_FLAG_SHARED  0x200000
#endif
#ifndef MAP_FLAG_PRIVATE
#define MAP_FLAG_PRIVATE 0x400000
#endif

static double diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec - a->tv_sec) * 1e9 + (double)(b->tv_nsec - a->tv_nsec);
}

/* Allocate and pointer-chase access, return GB/s (based on size_bytes) */
static double test_region(int use_cxl, size_t size_bytes, const char *name)
{
    void *buf;
    int flags;

    if (use_cxl) {
        flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FLAG_SHARED;
    } else {
        flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FLAG_PRIVATE;
    }

    buf = mmap(NULL, size_bytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (buf == MAP_FAILED || buf == NULL) {
        printf("%s: mmap failed (size=%zu)\n", name, size_bytes);
        return -1.0;
    }

    volatile uint64_t *next = (volatile uint64_t *)buf;

    /* Build pointer-chasing ring over cache-line-sized nodes */
    const size_t line_size = 64;
    const size_t elems_per_line = line_size / sizeof(uint64_t);
    size_t node_count = size_bytes / line_size;
    if (node_count < 2) {
        node_count = 2;
    }
    const uint64_t max_pos = (uint64_t)(node_count - 1) * elems_per_line;

    for (size_t i = 0; i < node_count; ++i) {
        next[i * elems_per_line] = (uint64_t)i;
    }

    for (size_t i = node_count - 1; i > 0; --i) {
        size_t j = (size_t)rand() % (i + 1);
        uint64_t tmp = next[i * elems_per_line];
        next[i * elems_per_line] = next[j * elems_per_line];
        next[j * elems_per_line] = tmp;
    }

    uint64_t first_pos = (uint64_t)(next[0] * elems_per_line);
    if (first_pos > max_pos) {
        printf("%s: corrupt first_pos=0x%lx > max 0x%lx\n", name, first_pos, max_pos);
        munmap((void *)buf, size_bytes);
        return -1.0;
    }
    uint64_t prev_pos = first_pos;
    for (size_t k = 1; k < node_count; ++k) {
        uint64_t cur_pos = (uint64_t)(next[k * elems_per_line] * elems_per_line);
        if (cur_pos > max_pos) {
            printf("%s: corrupt cur_pos=0x%lx at k=%zu > max 0x%lx\n", name, cur_pos, k, max_pos);
            munmap((void *)buf, size_bytes);
            return -1.0;
        }
        next[prev_pos] = cur_pos;
        prev_pos = cur_pos;
    }
    next[prev_pos] = first_pos;

    /* Warmup one full traversal */
    volatile uint64_t cur = first_pos;
    for (size_t k = 0; k < node_count; ++k) {
        if (cur > max_pos) {
            printf("%s: OOB cur=0x%lx > max 0x%lx at warmup step %zu\n", name, cur, max_pos, k);
            munmap((void *)buf, size_bytes);
            return -1.0;
        }
        cur = next[cur];
    }

    /* Timed traversals: data-dependent pointer chasing to reduce cache effectiveness */
    const size_t rounds = 8;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t k = 0; k < node_count; ++k) {
            if (cur > max_pos) {
                printf("%s: OOB cur=0x%lx > max 0x%lx in timed loop\n", name, cur, max_pos);
                munmap((void *)buf, size_bytes);
                return -1.0;
            }
            cur = next[cur];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    munmap(buf, size_bytes);

    double ns = diff_ns(&t0, &t1);
    double seconds = ns / 1e9;
    double gb = (double)size_bytes / (1024.0 * 1024.0 * 1024.0);
    double gbps = (seconds > 0) ? (gb / seconds) : 0.0;

    printf("%s: size=%.3f MB, time=%.3f ms, BW=%.2f GB/s\n",
           name, gb * 1024.0, seconds * 1e3, gbps);

    return gbps;
}

int main(int argc, char **argv)
{
    /* Default 64MB (safer for current kernel); use argv[1] to override */
    size_t size_bytes = (size_t)64 << 20;
    if (argc >= 2) {
        size_bytes = (size_t)strtoull(argv[1], NULL, 0);
        if (size_bytes == 0) {
            size_bytes = (size_t)64 << 20;
        }
    }

    printf("ChCore numa_bw_test: testing DRAM vs CXL bandwidth (size=%zu bytes, %.2f MB)\n",
           size_bytes, (double)size_bytes / (1024.0 * 1024.0));

    srand(12345); /* deterministic seed for pointer-chasing shuffle */

    test_region(0, size_bytes, "DRAM (MAP_FLAG_PRIVATE)");
    test_region(1, size_bytes, "CXL  (MAP_FLAG_SHARED)");

    printf("numa_bw_test done\n");
    return 0;
}
