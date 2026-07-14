/*
 * Test bandwidth of NUMA device files created by config_memdev.sh
 * (numa0.0, numa0.1, numa1.0, ... numa3.1 under /mnt/huge1G, 1G hugepage hugetlbfs)
 *
 * Compile: gcc -O2 -o numa_dev_bw_test numa_dev_bw_test.c
 * Run:     ./numa_dev_bw_test [test_size_mb] [bind_cpu]
 *          test_size_mb: MB to touch per device (default 1024, max = file size)
 *          bind_cpu: CPU to pin to (default: -1 = no pin)
 *
 * Prereq: sudo config_memdev.sh numa-setup-huge && config_memdev.sh numa-new
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

#define BASE_DIR     "/mnt/huge1G"
#define NUM_DEVICES  8
#define NS_IN_S      1000000000.0

static double diff_ns(const struct timespec *a, const struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1e9 + (b->tv_nsec - a->tv_nsec);
}

/* device index i -> name numa{j}.{k} where j=i/2, k=i%2 */
static void dev_name(int i, char *buf, size_t sz, const char *user)
{
    int j = i / 2, k = i % 2;
    snprintf(buf, sz, "%s/numa%d.%d-%s", BASE_DIR, j, k, user);
}

/* Sequential write over [p, p+size), return GB/s */
static double bw_write(volatile char *p, size_t size)
{
    struct timespec t0, t1;
    for (size_t i = 0; i < size; i += 64)
        p[i] = (char)(i & 0xff);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < size; i += 64)
        p[i]++;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double sec = diff_ns(&t0, &t1) / 1e9;
    return (size / (1024.0 * 1024.0 * 1024.0)) / sec;
}

/* Sequential read: sum to volatile to prevent opt-out */
static double bw_read(volatile char *p, size_t size)
{
    struct timespec t0, t1;
    volatile long sum = 0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (size_t i = 0; i < size; i += 64)
        sum += p[i];
    clock_gettime(CLOCK_MONOTONIC, &t1);
    (void)sum;
    double sec = diff_ns(&t0, &t1) / 1e9;
    return (size / (1024.0 * 1024.0 * 1024.0)) / sec;
}

static int test_one_dev(int dev_id, size_t test_bytes, int bind_cpu,
                        const char *user)
{
    char path[256];
    int fd;
    struct stat st;
    void *map;
    size_t map_len;
    double gbps_w, gbps_r;
    int numa_node = dev_id / 2;

    dev_name(dev_id, path, sizeof(path), user);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        perror("fstat");
        close(fd);
        return -1;
    }
    map_len = (size_t)st.st_size;
    if (test_bytes > 0 && test_bytes < map_len)
        map_len = test_bytes;

    map = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    if (bind_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(bind_cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
            perror("sched_setaffinity");
    }

    gbps_w = bw_write((volatile char *)map, map_len);
    gbps_r = bw_read((volatile char *)map, map_len);

    munmap(map, map_len);

    printf("numa%d.%d (node %d) %s: size=%.2f GB  write=%.2f GB/s  read=%.2f GB/s\n",
           dev_id / 2, dev_id % 2, numa_node, path,
           map_len / (1024.0 * 1024.0 * 1024.0), gbps_w, gbps_r);
    return 0;
}

int main(int argc, char **argv)
{
    const char *user = getenv("USER");
    size_t test_mb = 1024;
    int bind_cpu = -1;
    int i, err = 0;

    if (!user)
        user = "nobody";

    if (argc >= 2)
        test_mb = (size_t)atoi(argv[1]);
    if (argc >= 3)
        bind_cpu = atoi(argv[2]);

    size_t test_bytes = test_mb * (1024ULL * 1024);

    printf("NUMA device files under %s (user=%s), test size=%zu MB, bind_cpu=%d\n\n",
           BASE_DIR, user, test_mb, bind_cpu);

    for (i = 0; i < NUM_DEVICES; i++) {
        if (test_one_dev(i, test_bytes, bind_cpu, user) != 0)
            err++;
    }

    return err ? 1 : 0;
}
