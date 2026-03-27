#include "polling_req.h"
#include "polling_utils.h"
#include "polling_config.h"

#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

struct worker_thread_arg {
    struct polling_shm_region *shm;
    int fd;
    int tid;
    long diff_times[1000];
    int count;
};

#define WRITE_SIZE 1000
#define file_path  "test.txt"

static inline long diff_ns(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
}

void *worker_thread(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    char buf[WRITE_SIZE];
    struct timespec start, end;
    int count = 0;
    for (int i = 0; i < 1000; i++) {
        int fd = polling_fs_open(wta->shm, file_path, O_RDWR, 0);
        if (fd < 0) {
            printf("[thread %d] open failed at iter %d\n", wta->tid, i);
            break;
        }

#if PERF_READ == true
        clock_gettime(CLOCK_MONOTONIC, &start);
#endif
        ssize_t read_ret = polling_fs_read(wta->shm, fd, buf, WRITE_SIZE);
#if PERF_READ == true
        clock_gettime(CLOCK_MONOTONIC, &end);
        wta->diff_times[i] = diff_ns(start, end);
#endif
        if (read_ret != WRITE_SIZE) {
            printf("[thread %d] read %ld bytes, expected %d at iter %d\n",
                   wta->tid, read_ret, WRITE_SIZE, i);
        }
        polling_fs_close(wta->shm, fd);
        count++;
    }
    wta->count = count;
#if PERF_READ == true
    sort_long(wta->diff_times, count);
    printf("[thread %d] p50: %ld, p75: %ld, p90: %ld, p99: %ld (count=%d)\n",
           wta->tid,
           wta->diff_times[(int)(count * 0.50)],
           wta->diff_times[(int)(count * 0.75)],
           wta->diff_times[(int)(count * 0.90)],
           wta->diff_times[(int)(count * 0.99)],
           count);
#endif
    return NULL;
}

struct args {
    int shm_id;
    int num_threads;
};

void print_usage()
{
    printf("Usage: polling_client.bin -s <shm_id> [-t <num_threads>]\n");
}

void parse_args(int argc, char *argv[], struct args *args)
{
    args->shm_id = -1;
    args->num_threads = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            args->shm_id = atoi(argv[i + 1]);
            i += 1;
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            args->num_threads = atoi(argv[i + 1]);
            i += 1;
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            print_usage();
            exit(1);
        }
    }
    if (args->shm_id == -1) {
        args->shm_id = 0;
        printf("shm_id not provided, using default 0\n");
    }
}

int main(int argc, char *argv[])
{
    struct args args;
    parse_args(argc, argv, &args);

    void *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    int ret = usys_mmap_shm(args.shm_id, shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", args.shm_id);
        return -1;
    }
    struct polling_shm_region *shm = (struct polling_shm_region *)shm_addr;

    printf("[client] shm_id=%d threads=%d DQ_MAX_NODES=%d DQ_NODE_SIZE=%d\n",
           args.shm_id, args.num_threads,
           (int)DQ_MAX_NODES, (int)DQ_NODE_SIZE);

    /* Create and write the test file */
    int fd = polling_fs_open(shm, file_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("Failed to open file\n");
        return -1;
    }
    char buf[WRITE_SIZE];
    for (int i = 0; i < WRITE_SIZE; i++) {
        buf[i] = (char)(('a' + (char)(i % 26)));
    }
    ssize_t write_ret = polling_fs_write(shm, fd, buf, WRITE_SIZE);
    if (write_ret != WRITE_SIZE) {
        printf("write %ld bytes, expected %d\n", write_ret, WRITE_SIZE);
    }
    polling_fs_close(shm, fd);
    printf("[client] file created, sleeping 1s...\n");
    sleep(1);

    int num_threads = args.num_threads;
    printf("[client] creating %d worker threads\n", num_threads);

    pthread_t *tid = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    struct worker_thread_arg *wta =
        (struct worker_thread_arg *)malloc(sizeof(struct worker_thread_arg) * num_threads);
    if (!tid || !wta) {
        printf("Failed to allocate thread data\n");
        return -1;
    }

    for (int i = 0; i < num_threads; i++) {
        wta[i].shm = shm;
        wta[i].fd = fd;
        wta[i].tid = i;
        wta[i].count = 0;
        ret = pthread_create(&tid[i], NULL, worker_thread, (void *)&wta[i]);
        if (ret != 0) {
            printf("[client] pthread_create failed for thread %d, ret=%d\n", i, ret);
            num_threads = i;
            break;
        }
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(tid[i], NULL);
    }

#if PERF_READ == true
    /* Merge and print CDF data */
    int total = 0;
    for (int i = 0; i < num_threads; i++)
        total += wta[i].count;

    long *all = (long *)malloc(sizeof(long) * total);
    if (all) {
        int idx = 0;
        for (int i = 0; i < num_threads; i++) {
            for (int j = 0; j < wta[i].count; j++) {
                all[idx++] = wta[i].diff_times[j];
            }
        }
        sort_long(all, total);

        printf("[SUMMARY] total=%d threads=%d\n", total, num_threads);
        printf("[SUMMARY] p50: %ld, p75: %ld, p90: %ld, p99: %ld, max: %ld (ns)\n",
               all[(int)(total * 0.50)],
               all[(int)(total * 0.75)],
               all[(int)(total * 0.90)],
               all[(int)(total * 0.99)],
               all[total - 1]);

        printf("[CDF_BEGIN] count=%d\n", total);
        for (int i = 0; i < total; i++) {
            printf("[CDF] %d %ld\n", i, all[i]);
        }
        printf("[CDF_END]\n");

        free(all);
    }
#endif

    polling_print_debug_info(shm);
    debug_print_mpsc_alloc_msg_retry_time();
    printf("polling_client: done\n");

    free(tid);
    free(wta);
    return 0;
}
