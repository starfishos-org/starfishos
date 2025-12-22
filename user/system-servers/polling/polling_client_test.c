#include "polling.h"
#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <stdio.h>
#include <fcntl.h>

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
    ssize_t write_ret = polling_fs_write(shm, fd, "hello", 5);
    printf("wrote %ld bytes\n", write_ret);
    fd = polling_fs_open(shm, "test.txt", O_RDWR, 0666);
    if (fd < 0) {
        printf("Failed to open file\n");
        return -1;
    }
    printf("fd: %d\n", fd);
    char buf[10];
    ssize_t read_ret = polling_fs_read(shm, fd, buf, 5);
    printf("read %ld bytes\n", read_ret);
    printf("buf: %s\n", buf);
    polling_fs_close(shm, fd);
    printf("closed file\n");
    return 0;
}