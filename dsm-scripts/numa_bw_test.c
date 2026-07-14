// file: numa_bw_test.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <numa.h>
#include <numaif.h>
#include <unistd.h>

static double diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1e9 + (b->tv_nsec - a->tv_nsec);
}

// Allocate memory on the given NUMA node and build a pointer-chasing walk; return GB/s
double test_node(int node, size_t size_bytes, int cpu)
{
    if (numa_available() < 0) {
        fprintf(stderr, "numa not available\n");
        exit(1);
    }

    // Pin to CPU (optional, but recommended)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
    }

    // Allocate on the node
    void *buf = numa_alloc_onnode(size_bytes, node);
    if (!buf) {
        perror("numa_alloc_onnode");
        exit(1);
    }

    struct timespec t0, t1;
    volatile uint64_t *next = (volatile uint64_t *)buf;

    // Split at cache-line granularity and build the node count
    const size_t line_size = 64;
    const size_t elems_per_line = line_size / sizeof(uint64_t);
    size_t node_count = size_bytes / line_size;
    if (node_count < 2) {
        node_count = 2;
    }

    // Initialize as sequential indices
    for (size_t i = 0; i < node_count; ++i) {
        next[i * elems_per_line] = (uint64_t)i;
    }

    // Shuffle to form random access and avoid stride + prefetch
    for (size_t i = node_count - 1; i > 0; --i) {
        size_t j = (size_t)rand() % (i + 1);
        uint64_t tmp = next[i * elems_per_line];
        next[i * elems_per_line] = next[j * elems_per_line];
        next[j * elems_per_line] = tmp;
    }

    // Link shuffled indices into a ring: next[pos_k] = pos_{k+1}
    uint64_t first_pos = (uint64_t)(next[0] * elems_per_line);
    uint64_t prev_pos = first_pos;
    for (size_t k = 1; k < node_count; ++k) {
        uint64_t cur_pos = (uint64_t)(next[k * elems_per_line] * elems_per_line);
        next[prev_pos] = cur_pos;
        prev_pos = cur_pos;
    }
    next[prev_pos] = first_pos;

    // Warm-up: walk one lap
    volatile uint64_t cur = first_pos;
    // for (size_t k = 0; k < node_count; ++k) {
    //     cur = next[cur];
    // }

    // Timed measurement: multi-lap pointer chasing; each access depends on the prior load (hard to prefetch)
    const size_t rounds = 8;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t k = 0; k < node_count; ++k) {
            cur = next[cur];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = diff_ns(&t0, &t1);
    double seconds = ns / 1e9;
    double gb = (double)size_bytes / (1024.0 * 1024.0 * 1024.0);
    double gbps = gb / seconds;

    printf("node %d, cpu %d: size=%.2f GB, time=%.3f ms, BW=%.2f GB/s\n",
           node, cpu, gb, seconds * 1e3, gbps);

    numa_free(buf, size_bytes);
    return gbps;
}

int main(int argc, char **argv)
{
    if (numa_available() < 0) {
        fprintf(stderr, "This system does not support NUMA API\n");
        return 1;
    }

    int maxnode = numa_max_node();
    printf("max NUMA node: %d\n", maxnode);

    // Default 1GB; can be overridden via argv
    size_t size_bytes = (size_t)1 << 30;
    if (argc >= 2) {
        size_bytes = strtoull(argv[1], NULL, 0);
    }

    // Simple assumption: cpu == node
    for (int node = 0; node <= maxnode; ++node) {
        int cpu = node;  // Change to the CPU you want to pin to
        test_node(node, size_bytes, cpu);
    }

    return 0;
}