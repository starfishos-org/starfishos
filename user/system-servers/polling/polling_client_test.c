#include "polling_req.h"
#include "polling_utils.h"
#include "polling_config.h"

#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- Timing switch ----
 * Set ENABLE_TIMING to 1 to record per-iteration breakdown (rdtsc cycles).
 * Set to 0 when measuring CDF to eliminate timing overhead from the hot path.
 */
#define ENABLE_TIMING 0

#define WRITE_SIZE 4096  /* 4KiB file */
#define NUM_ITERS  1000
#define TEST_PATH  "test.txt"

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Per-iteration breakdown (only meaningful when ENABLE_TIMING == 1) */
struct iter_perf {
    uint64_t t_total;    /* full roundtrip (cycles) */
    uint64_t t_alloc;    /* dq_alloc_node (cycles) */
    uint64_t t_enqueue;  /* dq_enqueue (cycles) */
    uint64_t t_wait;     /* dq_wait_for_done (cycles) */
};

struct worker_thread_arg {
    struct polling_shm_region *shm; /* NULL = direct mode */
    int tid;
    int count;
    const char *mode_tag;           /* "direct" / "local" / "cross" */
    struct iter_perf perf[NUM_ITERS];
};

/* ---- Direct mode: normal IPC to tmpfs ---- */

void *worker_direct(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    char buf[WRITE_SIZE];
    int count = 0;

    for (int i = 0; i < NUM_ITERS; i++) {
        int fd = open(TEST_PATH, O_RDWR, 0);
        if (fd < 0) break;

#if ENABLE_TIMING
        uint64_t t0 = rdtsc();
#endif
        ssize_t n = read(fd, buf, WRITE_SIZE);
#if ENABLE_TIMING
        uint64_t t1 = rdtsc();
        wta->perf[i].t_total   = t1 - t0;
        wta->perf[i].t_alloc   = 0;
        wta->perf[i].t_enqueue = 0;
        wta->perf[i].t_wait    = 0;
#endif

        close(fd);
        if (n != WRITE_SIZE) {
            printf("[thread %d] direct read %ld at iter %d\n", wta->tid, n, i);
        }
        count++;
    }
    wta->count = count;
    return NULL;
}

/* ---- Polling mode: via durable queue ---- */

void *worker_empty_polling(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    struct polling_shm_region *shm = wta->shm;
    int count = 0;

    for (int i = 0; i < NUM_ITERS; i++) {
        struct polling_request req = { .type = POLLING_REQ_EMPTY };

#if ENABLE_TIMING
        uint64_t t0 = rdtsc();
#endif
        struct dq_node *node = dq_alloc_node(shm);
#if ENABLE_TIMING
        uint64_t t1 = rdtsc();
#endif
        dq_enqueue(shm, node, &req);
#if ENABLE_TIMING
        uint64_t t2 = rdtsc();
#endif
        dq_wait_for_done(node);
#if ENABLE_TIMING
        uint64_t t3 = rdtsc();
        wta->perf[i].t_alloc   = t1 - t0;
        wta->perf[i].t_enqueue = t2 - t1;
        wta->perf[i].t_wait    = t3 - t2;
        wta->perf[i].t_total   = t3 - t0;
#endif

        atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);
        count++;
    }
    wta->count = count;
    return NULL;
}

void *worker_polling(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    struct polling_shm_region *shm = wta->shm;
    char buf[WRITE_SIZE];
    int count = 0;

    for (int i = 0; i < NUM_ITERS; i++) {
        int fd = polling_fs_open(shm, TEST_PATH, O_RDWR, 0);
        if (fd < 0) {
            printf("[thread %d] open failed at iter %d\n", wta->tid, i);
            break;
        }

        size_t chunk = WRITE_SIZE < POLLING_FS_READ_BUF_SIZE
                     ? WRITE_SIZE : POLLING_FS_READ_BUF_SIZE;

        struct polling_request req = {
            .type = POLLING_FS_REQ_READ,
            .read = { .fd = fd, .count = chunk },
        };

#if ENABLE_TIMING
        uint64_t t0 = rdtsc();
#endif
        struct dq_node *node = dq_alloc_node(shm);
#if ENABLE_TIMING
        uint64_t t1 = rdtsc();
#endif
        dq_enqueue(shm, node, &req);
#if ENABLE_TIMING
        uint64_t t2 = rdtsc();
#endif
        dq_wait_for_done(node);
#if ENABLE_TIMING
        uint64_t t3 = rdtsc();
        wta->perf[i].t_alloc   = t1 - t0;
        wta->perf[i].t_enqueue = t2 - t1;
        wta->perf[i].t_wait    = t3 - t2;
        wta->perf[i].t_total   = t3 - t0;
#endif

        ssize_t n = node->resp.read.count;
        if (n > 0) memcpy(buf, node->resp.read.buf, n);
        atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);

        if (n != WRITE_SIZE) {
            printf("[thread %d] read %ld at iter %d\n", wta->tid, n, i);
        }

        polling_fs_close(shm, fd);
        count++;
    }
    wta->count = count;
    return NULL;
}

/* ---- Direct empty mode: empty IPC to tmpfs (no polling queue) ---- */

void *worker_direct_empty(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    int count = 0;

    /* Open file once to establish IPC connection */
    int fd = open(TEST_PATH, O_RDWR, 0);
    if (fd < 0) {
        wta->count = 0;
        return NULL;
    }

    for (int i = 0; i < NUM_ITERS; i++) {
#if ENABLE_TIMING
        uint64_t t0 = rdtsc();
#endif
        chcore_fs_noop(fd);
#if ENABLE_TIMING
        uint64_t t1 = rdtsc();
        wta->perf[i].t_total   = t1 - t0;
        wta->perf[i].t_alloc   = 0;
        wta->perf[i].t_enqueue = 0;
        wta->perf[i].t_wait    = 0;
#endif
        count++;
    }
    close(fd);
    wta->count = count;
    return NULL;
}

/* ---- Args ---- */

struct args {
    int shm_id;
    int num_threads;
    int direct;       /* 1 = direct IPC mode (no polling queue) */
    int empty;        /* 1 = POLLING_REQ_EMPTY only (no FS read), implies polling */
    const char *mode; /* "direct" / "local" / "cross" (for output tags) */
};

void print_usage()
{
    printf("Usage: polling_client.bin [-d] [-e] [-s <shm_id>] [-t <threads>] [-m <mode_tag>]\n");
    printf("  -d          direct mode (normal IPC, no polling queue)\n");
    printf("  -e          empty polling request (POLLING_REQ_EMPTY), no file I/O in loop\n");
    printf("  -s <id>     shared memory id (default 0)\n");
    printf("  -t <n>      number of worker threads (default 1)\n");
    printf("  -m <tag>    mode tag for output: direct/local/cross or t1/e1 etc.\n");
}

void parse_args(int argc, char *argv[], struct args *args)
{
    args->shm_id = 0;
    args->num_threads = 1;
    args->direct = 0;
    args->empty = 0;
    args->mode = "polling";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            args->direct = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            args->empty = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            args->shm_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            args->num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            args->mode = argv[++i];
        } else {
            print_usage();
            exit(1);
        }
    }
}

/* ---- Output results ---- */

static void print_results(struct worker_thread_arg *wta, int num_threads,
                          const char *mode)
{
    int total = 0;
    for (int i = 0; i < num_threads; i++)
        total += wta[i].count;
    if (total == 0) return;

    /* Collect all t_total values */
    uint64_t *all_total = (uint64_t *)malloc(sizeof(uint64_t) * total);
    int idx = 0;
    for (int i = 0; i < num_threads; i++)
        for (int j = 0; j < wta[i].count; j++)
            all_total[idx++] = wta[i].perf[j].t_total;
    sort_long((long *)all_total, total);

    printf("[SUMMARY] mode=%s total=%d threads=%d timing=%d\n",
           mode, total, num_threads, ENABLE_TIMING);
    printf("[SUMMARY] p50=%lu p75=%lu p90=%lu p99=%lu max=%lu (cycles)\n",
           all_total[(int)(total * 0.50)],
           all_total[(int)(total * 0.75)],
           all_total[(int)(total * 0.90)],
           all_total[(int)(total * 0.99)],
           all_total[total - 1]);

    /* CDF of t_total */
    printf("[CDF_BEGIN] mode=%s count=%d\n", mode, total);
    for (int i = 0; i < total; i++)
        printf("[CDF] %d %lu\n", i, all_total[i]);
    printf("[CDF_END]\n");
    free(all_total);

#if ENABLE_TIMING
    /* Breakdown data (all per-iteration samples, unsorted) */
    printf("[BREAKDOWN_BEGIN] mode=%s count=%d\n", mode, total);
    for (int i = 0; i < num_threads; i++)
        for (int j = 0; j < wta[i].count; j++)
            printf("[BD] %lu %lu %lu %lu\n",
                   wta[i].perf[j].t_total,
                   wta[i].perf[j].t_alloc,
                   wta[i].perf[j].t_enqueue,
                   wta[i].perf[j].t_wait);
    printf("[BREAKDOWN_END]\n");
#endif
}

/* ---- Main ---- */

int main(int argc, char *argv[])
{
    struct args args;
    parse_args(argc, argv, &args);

    struct polling_shm_region *shm = NULL;

    if (!args.direct) {
        void *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
        int ret = usys_mmap_shm(args.shm_id, shm_addr);
        if (ret < 0) {
            printf("Failed to mmap shm by id %d\n", args.shm_id);
            return -1;
        }
        shm = (struct polling_shm_region *)shm_addr;
    }

    printf("[client] mode=%s shm_id=%d threads=%d direct=%d empty=%d timing=%d\n",
           args.mode, args.shm_id, args.num_threads, args.direct, args.empty,
           ENABLE_TIMING);

    /* Create test file (needed for direct_empty to get valid fd for ipc_call) */
    if (args.direct) {
        int fd = open(TEST_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { printf("Failed to create file\n"); return -1; }
        if (!args.empty) {
            /* Write data for direct read test */
            char buf[WRITE_SIZE];
            for (int i = 0; i < WRITE_SIZE; i++)
                buf[i] = (char)('a' + (i % 26));
            write(fd, buf, WRITE_SIZE);
        }
        close(fd);
    } else {
        int fd = polling_fs_open(shm, TEST_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { printf("Failed to create file\n"); return -1; }
        if (!args.empty) {
            /* Write data for polling read test */
            char buf[WRITE_SIZE];
            for (int i = 0; i < WRITE_SIZE; i++)
                buf[i] = (char)('a' + (i % 26));
            polling_fs_write(shm, fd, buf, WRITE_SIZE);
        }
        polling_fs_close(shm, fd);
    }
    printf("[client] file created, sleeping 1s...\n");
    sleep(1);

    int nt = args.num_threads;
    pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t) * nt);
    struct worker_thread_arg *wta =
        (struct worker_thread_arg *)malloc(sizeof(struct worker_thread_arg) * nt);

    for (int i = 0; i < nt; i++) {
        wta[i].shm = shm;
        wta[i].tid = i;
        wta[i].count = 0;
        wta[i].mode_tag = args.mode;
        void *(*fn)(void *);
        if (args.direct)
            fn = args.empty ? worker_direct_empty : worker_direct;
        else if (args.empty)
            fn = worker_empty_polling;
        else
            fn = worker_polling;
        pthread_create(&tid[i], NULL, fn, &wta[i]);
    }
    for (int i = 0; i < nt; i++)
        pthread_join(tid[i], NULL);

    print_results(wta, nt, args.mode);

    if (!args.direct) {
        polling_print_debug_info(shm);
    }
    printf("polling_client: done\n");

    free(tid);
    free(wta);
    return 0;
}
