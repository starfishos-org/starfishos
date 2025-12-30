#include "polling_resp.h"
#include "polling.h"
#include "polling_utils.h"
#include "polling_config.h"

#include <chcore/syscall.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

long polling_request_time[50000];
int polling_request_time_index = 0;

void handle_polling_request(struct shm_msg *msg)
{
    struct timespec start_time, end_time;
#if PERF_REAL_READ == true
    clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif
    switch (msg->req.type) {
    case POLLING_FS_REQ_OPEN:
        handle_polling_fs_open(msg);
        break;
    case POLLING_FS_REQ_READ:
        handle_polling_fs_read(msg);
        break;
    case POLLING_FS_REQ_WRITE:
        handle_polling_fs_write(msg);
        break;
    case POLLING_FS_REQ_CLOSE:
        handle_polling_fs_close(msg);
        break;
    case POLLING_REQ_EMPTY:
        handle_polling_fs_empty(msg);
        break;
    case POLLING_KERNEL_REQ_FLUSH_TLB:
        handle_polling_kernel_flush_tlb(msg);
        break;
    case POLLING_PRINT_DEBUG_INFO:
        handle_polling_print_debug_info(msg);
        break;
    default:
        printf("Unsupported polling fs request type: %d\n", msg->req.type);
        break;
    }
#if PERF_REAL_READ == true
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    if (msg->req.type == POLLING_FS_REQ_READ) {
        polling_request_time[polling_request_time_index] =
                (end_time.tv_sec - start_time.tv_sec) * 1000000000
                + (end_time.tv_nsec - start_time.tv_nsec);
        polling_request_time_index++;
    }
#endif
}

void handle_polling_fs_open(struct shm_msg *msg)
{
    char path[FS_REQ_PATH_BUF_LEN];
    int flags = msg->req.open.flags;
    int mode = msg->req.open.mode;
    strncpy(path, msg->req.open.path, FS_REQ_PATH_BUF_LEN);

    int fd = open(path, flags, mode);

    msg->resp.open.fd = fd;
}

void handle_polling_fs_read(struct shm_msg *msg)
{
    int fd = msg->req.read.fd;
    size_t count = msg->req.read.count;

    ssize_t ret = read(fd, msg->resp.read.buf, count);

    msg->resp.read.count = ret;
}

void handle_polling_fs_write(struct shm_msg *msg)
{
    int fd = msg->req.write.fd;
    size_t count = msg->req.write.count;

    ssize_t ret = write(fd, msg->req.write.buf, count);

    msg->resp.write.count = ret;
}

void handle_polling_fs_close(struct shm_msg *msg)
{
    int fd = msg->req.close.fd;

    int ret = close(fd);

    msg->resp.close.ret = ret;
}

void handle_polling_fs_empty(struct shm_msg *msg)
{
    // do nothing
}

void handle_polling_kernel_flush_tlb(struct shm_msg *msg)
{
    u64 memcpy_src_pa = msg->req.flush_tlb.memcpy_src_pa;
    u64 memcpy_dst_pa = msg->req.flush_tlb.memcpy_dst_pa;
    u64 memcpy_len = msg->req.flush_tlb.memcpy_len;
    u64 memcpy_fault_va = msg->req.flush_tlb.memcpy_fault_va;
    u64 memcpy_vmspace = msg->req.flush_tlb.memcpy_vmspace;

    int ret = usys_memcpy_and_flush_tlb(memcpy_src_pa,
                                        memcpy_dst_pa,
                                        memcpy_len,
                                        memcpy_fault_va,
                                        memcpy_vmspace);
    extern int my_id;
    assert(my_id >= 0);
    msg->resp.flush_tlb.reply_result = ret;
    msg->resp.flush_tlb.reply_from = my_id;
    msg->resp.flush_tlb.reply_received = 1;
}

void handle_polling_print_debug_info(struct shm_msg *msg)
{
#if PERF_REAL_READ == true
    long *tmp = (long *)malloc(sizeof(long) * polling_request_time_index);
    memcpy(tmp,
           polling_request_time,
           sizeof(long) * polling_request_time_index);
    sort_long(tmp, polling_request_time_index);
    printf("polling_request_time: %d p50: %ld, p75: %ld, p90: %ld, p99: %ld\n",
           polling_request_time_index,
           tmp[(int)(polling_request_time_index * 0.50)],
           tmp[(int)(polling_request_time_index * 0.75)],
           tmp[(int)(polling_request_time_index * 0.90)],
           tmp[(int)(polling_request_time_index * 0.99)]);
    free(tmp);
#endif
}