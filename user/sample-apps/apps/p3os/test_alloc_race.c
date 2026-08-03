/*
 * test_alloc_race - does malloc hand the same memory to two owners at once?
 *
 * PCA's covariance phase emits ~500k 8-byte keys (pca_cov_loc_t, a pair of row
 * indices) from 64 threads spread over 8 machines.  Its 8-machine crashes look
 * like those 8 bytes landing on top of live memory: worker threads jump to an
 * address whose two halves are both valid row indices (0..999), i.e. a
 * pca_cov_loc_t used as a return address, and a Phoenix task-queue list node
 * was seen with one 8-byte field overwritten.  Phoenix never copies key
 * *contents* around -- it stores the pointer -- so the suspicion is that the
 * allocation itself overlaps memory somebody else already owns.
 *
 * This isolates that: every thread repeatedly allocates small blocks, stamps
 * each with a canary only it could have written, and re-checks every canary
 * before freeing.  A mismatch means the block was simultaneously owned by
 * someone else.  Nothing here depends on Phoenix or MapReduce.
 *
 * Usage: test_alloc_race.bin [threads] [rounds] [blocks_per_round] [size]
 *
 * Threads are pinned to the same CPU pattern the benchmarks use
 * (0-7, 12-19, 24-31, ...), so thread i runs on machine i/8.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_THREADS      128
#define MAX_BLOCKS       4096
#define CPUS_PER_MACHINE 12
#define WORKERS_PER_MACHINE 8

static int n_threads = 64;
static int n_rounds = 200;
static int n_blocks = 256;
static size_t blk_size = 8;

static volatile unsigned long total_bad;
static volatile unsigned long total_ops;
static volatile int start_gate;

struct canary {
    uint32_t owner;   /* thread index */
    uint32_t serial;  /* allocation serial within this thread */
};

static void *worker(void *arg)
{
    long tid = (long)arg;
    void **blocks;
    cpu_set_t set;
    int global_cpu;
    long round, i;
    unsigned long bad = 0, ops = 0;
    uint32_t serial = 0;

    global_cpu = (tid / WORKERS_PER_MACHINE) * CPUS_PER_MACHINE
                 + (tid % WORKERS_PER_MACHINE);
    CPU_ZERO(&set);
    CPU_SET(global_cpu, &set);
    /* -2: global affinity, migrates this thread to that CPU's machine. */
    sched_setaffinity(-2, sizeof(set), &set);
    sched_yield();

    blocks = malloc(sizeof(void *) * n_blocks);
    if (blocks == NULL)
        return NULL;

    while (start_gate == 0)
        sched_yield();

    for (round = 0; round < n_rounds; round++) {
        /* Allocate a batch and stamp each block. */
        for (i = 0; i < n_blocks; i++) {
            struct canary *c = malloc(blk_size);

            blocks[i] = c;
            if (c == NULL)
                continue;
            c->owner = (uint32_t)tid;
            c->serial = serial++;
        }

        /* Nobody else may have touched them. */
        for (i = 0; i < n_blocks; i++) {
            struct canary *c = blocks[i];

            if (c == NULL)
                continue;
            ops++;
            if (c->owner != (uint32_t)tid) {
                if (bad < 4) {
                    printf("[alloc-race] MISMATCH tid=%ld cpu=%d block=%p "
                           "expected owner=%ld, found owner=%u serial=%u\n",
                           tid, global_cpu, (void *)c, tid,
                           c->owner, c->serial);
                }
                bad++;
            }
        }

        for (i = 0; i < n_blocks; i++)
            free(blocks[i]);
    }

    free(blocks);
    __atomic_fetch_add(&total_bad, bad, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&total_ops, ops, __ATOMIC_SEQ_CST);
    return NULL;
}

int main(int argc, char *argv[])
{
    pthread_t tids[MAX_THREADS];
    long i;

    if (argc > 1)
        n_threads = atoi(argv[1]);
    if (argc > 2)
        n_rounds = atoi(argv[2]);
    if (argc > 3)
        n_blocks = atoi(argv[3]);
    if (argc > 4)
        blk_size = (size_t)atoi(argv[4]);

    if (n_threads > MAX_THREADS)
        n_threads = MAX_THREADS;
    if (n_blocks > MAX_BLOCKS)
        n_blocks = MAX_BLOCKS;
    if (blk_size < sizeof(struct canary))
        blk_size = sizeof(struct canary);

    printf("[alloc-race] %d threads x %d rounds x %d blocks of %zu bytes\n",
           n_threads, n_rounds, n_blocks, blk_size);

    for (i = 0; i < n_threads; i++) {
        if (pthread_create(&tids[i], NULL, worker, (void *)i) != 0) {
            printf("[alloc-race] FAILED: pthread_create %ld\n", i);
            return 1;
        }
    }

    start_gate = 1;
    for (i = 0; i < n_threads; i++)
        pthread_join(tids[i], NULL);

    printf("[alloc-race] checked %lu block(s), %lu bad\n",
           total_ops, total_bad);
    if (total_bad)
        printf("[alloc-race] RESULT: OVERLAP - malloc handed live memory to "
               "more than one owner\n");
    else
        printf("[alloc-race] RESULT: OK - no block was owned twice\n");
    printf("[alloc-race] done\n");
    return total_bad ? 1 : 0;
}
