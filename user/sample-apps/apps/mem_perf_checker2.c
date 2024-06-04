/*
 * numa_memory_latency
 * Copyright (c) 2017 UMEZAWA Takeshi
 * This software is licensed under GNU GPL version 2 or later.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>

#ifdef NUMA
#include <numa.h>
#endif

#define cachelinesize 64
union CACHELINE {
    char cacheline[cachelinesize];
    volatile union CACHELINE* next;
};

#define REPT4(x)    do { x; x; x; x; } while(0)
#define REPT16(x)   do { REPT4(x);   REPT4(x);   REPT4(x);   REPT4(x);   } while(0);
#define REPT64(x)   do { REPT16(x);  REPT16(x);  REPT16(x);  REPT16(x);  } while(0);
#define REPT256(x)  do { REPT64(x);  REPT64(x);  REPT64(x);  REPT64(x);  } while(0);
#define REPT1024(x) do { REPT256(x); REPT256(x); REPT256(x); REPT256(x); } while(0);

size_t bufsize = 128 * 1024 * 1024;
size_t nloop = 128 * 1024;

size_t *offsets;

volatile union CACHELINE* walk(volatile union CACHELINE* start)
{
    volatile union CACHELINE* p = start;
    for (size_t i = 0; i < nloop; ++i) {
        REPT1024(p = p->next);
    }
    return p;
}

void bench(int tasknode, int memnode)
{
    struct timespec ts_begin, ts_end, ts_elapsed;

#ifdef NUMA
    printf("NUMA: bench(task=%d, mem=%d)\n", tasknode, memnode);

    if (numa_run_on_node(tasknode) != 0) {
        printf("failed to run on node: %s\n", strerror(errno));
        return;
    }
    union CACHELINE* const buf = (union CACHELINE*)numa_alloc_onnode(bufsize, memnode);
#else 
    printf("NO NUMA: bench(task=%d, mem=%d)\n", tasknode, memnode);

    union CACHELINE* const buf = (union CACHELINE*)malloc(bufsize);
#endif

    if (buf == NULL) {
        printf("failed to allocate memory\n");
        return;
    }

	printf("Preparing buffer data...\n");
    for (size_t i = 0; i < bufsize / cachelinesize - 1; ++i) {
        buf[offsets[i]].next = buf + offsets[i+1];
    }
    buf[offsets[bufsize / cachelinesize - 1]].next = buf;

	printf("Start Walking...\n");
    clock_gettime(CLOCK_MONOTONIC, &ts_begin);
    walk(buf);
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    ts_elapsed.tv_nsec = ts_end.tv_nsec - ts_begin.tv_nsec;
    ts_elapsed.tv_sec = ts_end.tv_sec - ts_begin.tv_sec;
    if (ts_elapsed.tv_nsec < 0) {
        --ts_elapsed.tv_sec;
        ts_elapsed.tv_nsec += 1000*1000*1000;
    }
    double elapsed = ts_elapsed.tv_sec + 0.000000001 * ts_elapsed.tv_nsec;
    printf("took %fsec. %fns/load\n", elapsed, elapsed/(1024*nloop)*(1000*1000*1000));

#ifdef NUMA
    numa_free(buf, bufsize);
#else
    free(buf);
#endif
}

void usage(const char* prog)
{
    printf("usage: %s [-h] [bufsize] [nloop]\n", prog);
}

int main(int argc, char* argv[])
{
    int ch;

    while ((ch = getopt(argc, argv, "h")) != -1) {
        switch (ch) {
        case 'h':
        default:
            usage(argv[0]);
            exit(1);
        }
    }

    argc -= optind;
    argv += optind;

    if (argc > 0)
        bufsize = atoi(argv[0]) * 1024;
    if (argc > 1)
        nloop = atoi(argv[1]) * 1024;

    offsets = (size_t*)malloc(bufsize / cachelinesize * sizeof(size_t));
    if (offsets == NULL) {
        printf("failed to allocate memory for offsets\n");
        return 1;
    }

    for (size_t i = 0; i < bufsize / cachelinesize; ++i)
        offsets[i] = i;

    srand(time(NULL));
    for (size_t i = 1; i < bufsize / cachelinesize; ++i) {
        size_t j = 1 + rand() % (bufsize / cachelinesize - 1);
        size_t temp = offsets[i];
        offsets[i] = offsets[j];
        offsets[j] = temp;
    }

    printf("benchmark bufsize=%zuKiB, nloop=%zuKi\n", bufsize/1024, nloop/1024);

    int tasknode = 0;
    int memnode = 0;
    bench(tasknode, memnode);

    free(offsets);
    return 0;
}
