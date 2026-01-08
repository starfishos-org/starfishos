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
    long diff_times[1000];
    for (int i = 0; i < 1000; i++) {
        int fd = polling_fs_open(wta->shm, file_path, O_RDWR, 0);

#if PERF_READ == true
        clock_gettime(CLOCK_MONOTONIC, &start);
#endif
        ssize_t read_ret = polling_fs_read(wta->shm, fd, buf, WRITE_SIZE);
#if PERF_READ == true
        clock_gettime(CLOCK_MONOTONIC, &end);
        diff_times[i] = diff_ns(start, end);
#endif
        if (read_ret != WRITE_SIZE) {
            printf("read %ld bytes, expected %d\n", read_ret, WRITE_SIZE);
        }
        polling_fs_close(wta->shm, fd);
    }
#if PERF_READ == true
    sort_long(diff_times, 1000);
    printf("p50: %ld, p75: %ld, p90: %ld, p99: %ld\n",
           diff_times[(int)(1000 * 0.50)],
           diff_times[(int)(1000 * 0.75)],
           diff_times[(int)(1000 * 0.90)],
           diff_times[(int)(1000 * 0.99)]);
#endif
    return NULL;
}

struct args {
    int shm_id;
};

void print_usage()
{
    printf("Usage: polling_client.bin -s <shm_id>\n");
}

void parse_args(int argc, char *argv[], struct args *args)
{
    args->shm_id = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            args->shm_id = atoi(argv[i + 1]);
            i += 1;
        } else {
            printf("Unknown argument: %s\n", argv[i]);
            print_usage();
            exit(1);
        }
    }
    if (args->shm_id == -1) {
        args->shm_id = 0;
        print_usage();
        printf("shm_id is not provided, using default 0\n");
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
    sleep(1);
    const int num_threads = 10;
    pthread_t tid[num_threads];
    struct worker_thread_arg wta[num_threads];
    for (int i = 0; i < num_threads; i++) {
        wta[i].shm = shm;
        wta[i].fd = fd;
        wta[i].tid = i;
        pthread_create(&tid[i], NULL, worker_thread, (void *)&wta[i]);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(tid[i], NULL);
    }
    polling_print_debug_info(shm);
    debug_print_mpsc_alloc_msg_retry_time();
    return 0;
}