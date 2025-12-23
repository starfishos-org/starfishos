#include "polling_req.h"

#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <stdio.h>
#include <fcntl.h>

struct worker_thread_arg {
    struct polling_shm_region *shm;
    int fd;
    int tid;
};

void *worker_thread(void *arg)
{
    struct worker_thread_arg *wta = (struct worker_thread_arg *)arg;
    char buf[10];
    sprintf(buf, "hello%d", wta->tid);
    for (int i = 0; i < 10; i++) {
        sprintf(buf, "hello%d-%d\n", wta->tid, i);
        polling_fs_write(wta->shm, wta->fd, buf, strlen(buf));
    }
    return NULL;
}

static inline long diff_ns(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) * 1000000000L +
           (b.tv_nsec - a.tv_nsec);
}

int main(int argc, char *argv[])
{
    void *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    int ret = usys_mmap_shm(POLLING_FS_SHM_ID, shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", POLLING_FS_SHM_ID);
        return -1;
    }
    struct polling_shm_region *shm = (struct polling_shm_region *)shm_addr;
    int fd = polling_fs_open(shm, "test.txt", O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        printf("Failed to open file\n");
        return -1;
    }
    printf("fd: %d\n", fd);
    pthread_t tid[10];
    struct worker_thread_arg wta[10];
    for (int i = 0; i < 10; i++) {
        wta[i].shm = shm;
        wta[i].fd = fd;
        wta[i].tid = i;
        pthread_create(&tid[i], NULL, worker_thread, (void *)&wta[i]);
    }
    for (int i = 0; i < 10; i++) {
        pthread_join(tid[i], NULL);
    }
    fd = polling_fs_open(shm, "test.txt", O_RDWR, 0666);
    if (fd < 0) {
        printf("Failed to open file\n");
        return -1;
    }
    printf("fd: %d\n", fd);
    char buf[1000];
    ssize_t read_ret = polling_fs_read(shm, fd, buf, 1000);
    printf("read %ld bytes\n", read_ret);
    polling_fs_close(shm, fd);
    printf("closed file\n");

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < 1000; i++) {
        polling_fs_empty(shm);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("polling_fs_empty 1k times cost: %ld ns average: %ld ns\n", diff_ns(start, end), diff_ns(start, end) / 1000);
    return 0;
}