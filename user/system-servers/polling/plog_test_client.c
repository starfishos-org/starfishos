#include "polling_req.h"
#include "polling_config.h"

#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PATH     "plog_test.txt"
#define TEST_DATA_SIZE 512

static const char test_pattern[] = "ANANKE_RECOVERY_TEST_";

static void fill_test_data(char *buf, size_t size)
{
    size_t plen = strlen(test_pattern);
    for (size_t i = 0; i < size; i++)
        buf[i] = test_pattern[i % plen];
}

static int verify_test_data(const char *buf, size_t size)
{
    size_t plen = strlen(test_pattern);
    for (size_t i = 0; i < size; i++) {
        if (buf[i] != test_pattern[i % plen])
            return 0;
    }
    return 1;
}

struct args {
    int shm_id;
    const char *mode; /* "write" or "verify" */
};

void parse_args(int argc, char *argv[], struct args *args)
{
    args->shm_id = 0;
    args->mode = "write";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            args->shm_id = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            args->mode = argv[++i];
        }
    }
}

int main(int argc, char *argv[])
{
    struct args args;
    parse_args(argc, argv, &args);

    void *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    int ret = usys_mmap_shm(args.shm_id, shm_addr);
    if (ret < 0) {
        printf("[plog_test] Failed to mmap shm %d\n", args.shm_id);
        return -1;
    }
    struct polling_shm_region *shm = (struct polling_shm_region *)shm_addr;

    printf("[plog_test] mode=%s shm_id=%d\n", args.mode, args.shm_id);

    if (strcmp(args.mode, "write") == 0) {
        /* Create file and write test data */
        char buf[TEST_DATA_SIZE];
        fill_test_data(buf, TEST_DATA_SIZE);

        int fd = polling_fs_open(shm, TEST_PATH,
                                 O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            printf("[plog_test] Failed to create file\n");
            return -1;
        }
        ssize_t n = polling_fs_write(shm, fd, buf, TEST_DATA_SIZE);
        printf("[PLOG_TEST] wrote %ld bytes\n", n);
        polling_fs_close(shm, fd);
        printf("plog_test: write_done\n");

    } else if (strcmp(args.mode, "verify") == 0) {
        /* Open file and verify data */
        int fd = polling_fs_open(shm, TEST_PATH, O_RDONLY, 0);
        if (fd < 0) {
            printf("[PLOG_TEST] data_match=NO (file not found)\n");
            printf("plog_test: verify_done\n");
            return -1;
        }
        char buf[TEST_DATA_SIZE];
        memset(buf, 0, TEST_DATA_SIZE);
        ssize_t n = polling_fs_read(shm, fd, buf, TEST_DATA_SIZE);
        polling_fs_close(shm, fd);

        int match = (n == TEST_DATA_SIZE) && verify_test_data(buf, n);
        printf("[PLOG_TEST] data_match=%s size=%ld\n",
               match ? "YES" : "NO", n);
        printf("plog_test: verify_done\n");
    } else {
        printf("[plog_test] Unknown mode: %s\n", args.mode);
        return -1;
    }

    return 0;
}
