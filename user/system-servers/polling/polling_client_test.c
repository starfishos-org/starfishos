#include "polling_req.h"
#include "polling_utils.h"
#include "polling_config.h"

#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <chcore/launcher.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/shm.h>
#include <sys/wait.h>

/* SHM key used to share wta[] between parent and worker processes */
#define WTA_SHM_KEY 0xC1E01

/* ---- Timing configuration ----
 * Timing is always collected for CDF (cycles). Use flags to control output:
 * - ENABLE_BREAKDOWN: Output per-component breakdown [BREAKDOWN_BEGIN/END]
 *   (alloc, enqueue, wait times). Set to 1 for detailed, 0 for CDF only.
 */
#define ENABLE_BREAKDOWN 0

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

    int fd = open(TEST_PATH, O_RDWR, 0);
    if (fd < 0) { wta->count = 0; return NULL; }

    for (int i = 0; i < NUM_ITERS; i++) {
        lseek(fd, 0, SEEK_SET);

        uint64_t t0 = rdtsc();
        ssize_t n = read(fd, buf, WRITE_SIZE);
        uint64_t t1 = rdtsc();
        wta->perf[i].t_total   = t1 - t0;
        wta->perf[i].t_alloc   = 0;
        wta->perf[i].t_enqueue = 0;
        wta->perf[i].t_wait    = 0;
        if (n != WRITE_SIZE) {
            printf("[thread %d] direct read %ld at iter %d\n", wta->tid, n, i);
        }
        count++;
    }
    close(fd);
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

        uint64_t t0 = rdtsc();
        struct dq_node *node = dq_alloc_node(shm);
        uint64_t t1 = rdtsc();
        dq_enqueue(shm, node, &req);
        uint64_t t2 = rdtsc();
        dq_wait_for_done(node);
        uint64_t t3 = rdtsc();
        wta->perf[i].t_alloc   = t1 - t0;
        wta->perf[i].t_enqueue = t2 - t1;
        wta->perf[i].t_wait    = t3 - t2;
        wta->perf[i].t_total   = t3 - t0;
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

        uint64_t t0 = rdtsc();
        struct dq_node *node = dq_alloc_node(shm);
        uint64_t t1 = rdtsc();
        dq_enqueue(shm, node, &req);
        uint64_t t2 = rdtsc();
        dq_wait_for_done(node);
        uint64_t t3 = rdtsc();
        wta->perf[i].t_alloc   = t1 - t0;
        wta->perf[i].t_enqueue = t2 - t1;
        wta->perf[i].t_wait    = t3 - t2;
        wta->perf[i].t_total   = t3 - t0;
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

/* ---- Direct write mode: normal IPC write to tmpfs ---- */

void *worker_direct_write(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    char buf[WRITE_SIZE];
    int count = 0;

    for (int i = 0; i < WRITE_SIZE; i++)
        buf[i] = (char)('a' + (i % 26));

    int fd = open(TEST_PATH, O_RDWR, 0);
    if (fd < 0) { wta->count = 0; return NULL; }

    for (int i = 0; i < NUM_ITERS; i++) {
        lseek(fd, 0, SEEK_SET);

        uint64_t t0 = rdtsc();
        ssize_t n = write(fd, buf, WRITE_SIZE);
        uint64_t t1 = rdtsc();
        wta->perf[i].t_total   = t1 - t0;
        wta->perf[i].t_alloc   = 0;
        wta->perf[i].t_enqueue = 0;
        wta->perf[i].t_wait    = 0;
        if (n != WRITE_SIZE) {
            printf("[thread %d] direct write %ld at iter %d\n", wta->tid, n, i);
        }
        count++;
    }
    close(fd);
    wta->count = count;
    return NULL;
}

/* ---- Polling write mode: via durable queue ---- */

void *worker_polling_write(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    struct polling_shm_region *shm = wta->shm;
    char buf[WRITE_SIZE];
    int count = 0;

    for (int i = 0; i < WRITE_SIZE; i++)
        buf[i] = (char)('a' + (i % 26));

    for (int i = 0; i < NUM_ITERS; i++) {
        int fd = polling_fs_open(shm, TEST_PATH, O_RDWR, 0);
        if (fd < 0) {
            printf("[thread %d] open failed at iter %d\n", wta->tid, i);
            break;
        }

        size_t chunk = WRITE_SIZE < POLLING_FS_WRITE_BUF_SIZE
                     ? WRITE_SIZE : POLLING_FS_WRITE_BUF_SIZE;

        struct polling_request req = {
            .type = POLLING_FS_REQ_WRITE,
            .write = { .fd = fd, .count = chunk },
        };
        memcpy(req.write.buf, buf, chunk);

        uint64_t t0 = rdtsc();
        struct dq_node *node = dq_alloc_node(shm);
        uint64_t t1 = rdtsc();
        dq_enqueue(shm, node, &req);
        uint64_t t2 = rdtsc();
        dq_wait_for_done(node);
        uint64_t t3 = rdtsc();
        wta->perf[i].t_alloc   = t1 - t0;
        wta->perf[i].t_enqueue = t2 - t1;
        wta->perf[i].t_wait    = t3 - t2;
        wta->perf[i].t_total   = t3 - t0;
        atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);

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
        uint64_t t0 = rdtsc();
        chcore_fs_noop(fd);
        uint64_t t1 = rdtsc();
        wta->perf[i].t_total   = t1 - t0;
        wta->perf[i].t_alloc   = 0;
        wta->perf[i].t_enqueue = 0;
        wta->perf[i].t_wait    = 0;
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
    int write_mode;   /* 1 = write 4KiB instead of read */
    const char *mode; /* "direct" / "local" / "cross" (for output tags) */
    int worker_id;    /* >= 0: running as child worker with this index */
};

void print_usage()
{
    printf("Usage: polling_client.bin [-d] [-e] [-w] [-s <shm_id>] [-t <threads>] [-m <mode_tag>]\n");
    printf("  -d          direct mode (normal IPC, no polling queue)\n");
    printf("  -e          empty polling request (POLLING_REQ_EMPTY), no file I/O in loop\n");
    printf("  -w          write 4KiB instead of read\n");
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
    args->write_mode = 0;
    args->mode = "polling";
    args->worker_id = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            args->direct = 1;
        } else if (strcmp(argv[i], "-e") == 0) {
            args->empty = 1;
        } else if (strcmp(argv[i], "-w") == 0) {
            args->write_mode = 1;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            args->shm_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            args->num_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            args->mode = argv[++i];
        } else if (strcmp(argv[i], "-W") == 0 && i + 1 < argc) {
            args->worker_id = atoi(argv[++i]);
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

    printf("[SUMMARY] mode=%s total=%d threads=%d breakdown=%d\n",
           mode, total, num_threads, ENABLE_BREAKDOWN);
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
}

/* ---- Main ---- */

static struct polling_shm_region *map_polling_shm(int shm_id)
{
    void *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    int ret = usys_mmap_shm(shm_id, shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", shm_id);
        return NULL;
    }
    return (struct polling_shm_region *)shm_addr;
}

static void *(*select_fn(struct args *args))(void *)
{
    if (args->direct)
        return args->empty ? worker_direct_empty
             : args->write_mode ? worker_direct_write
             : worker_direct;
    if (args->empty)
        return worker_empty_polling;
    if (args->write_mode)
        return worker_polling_write;
    return worker_polling;
}

int main(int argc, char *argv[])
{
    struct args args;
    parse_args(argc, argv, &args);

    int nt = args.num_threads;
    size_t wta_shm_size = sizeof(struct worker_thread_arg) * nt;

    /* ----- Worker mode: child process spawned by parent ----- */
    if (args.worker_id >= 0) {
        struct polling_shm_region *shm = NULL;
        if (!args.direct) {
            shm = map_polling_shm(args.shm_id);
            if (!shm) return -1;
        }

        int wta_shmid = shmget(WTA_SHM_KEY, wta_shm_size, IPC_CREAT);
        struct worker_thread_arg *wta =
            (struct worker_thread_arg *)shmat(wta_shmid, 0, 0);
        wta[args.worker_id].shm = shm;

        select_fn(&args)(&wta[args.worker_id]);
        return 0;
    }

    /* ----- Parent mode ----- */
    struct polling_shm_region *shm = NULL;
    if (!args.direct) {
        shm = map_polling_shm(args.shm_id);
        if (!shm) return -1;
    }

    printf("[client] mode=%s shm_id=%d procs=%d direct=%d empty=%d write=%d breakdown=%d\n",
           args.mode, args.shm_id, nt, args.direct, args.empty,
           args.write_mode, ENABLE_BREAKDOWN);

    /* Create test file */
    if (args.direct) {
        int fd = open(TEST_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { printf("Failed to create file\n"); return -1; }
        if (!args.empty) {
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
            char buf[WRITE_SIZE];
            for (int i = 0; i < WRITE_SIZE; i++)
                buf[i] = (char)('a' + (i % 26));
            polling_fs_write(shm, fd, buf, WRITE_SIZE);
        }
        polling_fs_close(shm, fd);
    }
    printf("[client] file created, sleeping 1s...\n");
    sleep(1);

    /* Create shared wta[] for result collection across processes */
    int wta_shmid = shmget(WTA_SHM_KEY, wta_shm_size, IPC_CREAT | IPC_EXCL);
    struct worker_thread_arg *wta =
        (struct worker_thread_arg *)shmat(wta_shmid, 0, 0);
    for (int i = 0; i < nt; i++) {
        wta[i].tid = i;
        wta[i].count = 0;
        wta[i].mode_tag = args.mode;
    }

    /* Spawn worker processes: re-launch this binary with -W <i> appended */
    pid_t *pids = (pid_t *)malloc(sizeof(pid_t) * nt);
    for (int i = 0; i < nt; i++) {
        char wi_str[16];
        snprintf(wi_str, sizeof(wi_str), "%d", i);
        char **child_argv = (char **)malloc(sizeof(char *) * (argc + 3));
        for (int j = 0; j < argc; j++)
            child_argv[j] = argv[j];
        child_argv[argc]     = "-W";
        child_argv[argc + 1] = wi_str;
        child_argv[argc + 2] = NULL;
        pids[i] = chcore_new_process(argc + 2, child_argv, 0, 0);
        free(child_argv);
    }

    for (int i = 0; i < nt; i++)
        waitpid(pids[i], NULL, 0);

    print_results(wta, nt, args.mode);

    if (!args.direct)
        polling_print_debug_info(shm);
    printf("polling_client: done\n");

    free(pids);
    shmdt(wta);
    shmctl(wta_shmid, IPC_RMID, 0);
    return 0;
}
