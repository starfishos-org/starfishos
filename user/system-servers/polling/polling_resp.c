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

void handle_polling_request(struct dq_node *node)
{
    struct timespec start_time, end_time;
#if PERF_REAL_READ == true
    clock_gettime(CLOCK_MONOTONIC, &start_time);
#endif
    switch (node->req.type) {
    case POLLING_FS_REQ_OPEN:
        handle_polling_fs_open(node);
        break;
    case POLLING_FS_REQ_READ:
        handle_polling_fs_read(node);
        break;
    case POLLING_FS_REQ_WRITE:
        handle_polling_fs_write(node);
        break;
    case POLLING_FS_REQ_CLOSE:
        handle_polling_fs_close(node);
        break;
    case POLLING_REQ_EMPTY:
        handle_polling_fs_empty(node);
        break;
    case POLLING_KERNEL_REQ_FLUSH_TLB:
        handle_polling_kernel_flush_tlb(node);
        break;
    case POLLING_PRINT_DEBUG_INFO:
        handle_polling_print_debug_info(node);
        break;
    default:
        printf("Unsupported polling request type: %d\n", node->req.type);
        break;
    }
#if PERF_REAL_READ == true
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    if (node->req.type == POLLING_FS_REQ_READ) {
        polling_request_time[polling_request_time_index] =
                (end_time.tv_sec - start_time.tv_sec) * 1000000000
                + (end_time.tv_nsec - start_time.tv_nsec);
        polling_request_time_index++;
    }
#endif
}

/*
 * Note: these handlers first read fields from node->req (the request),
 * then write the result into node->resp (the response).
 * Since req and resp share a union, the handler MUST read all needed
 * request fields before writing any response fields.
 */

void handle_polling_fs_open(struct dq_node *node)
{
    char path[FS_REQ_PATH_BUF_LEN];
    int flags = node->req.open.flags;
    int mode = node->req.open.mode;
    strncpy(path, node->req.open.path, FS_REQ_PATH_BUF_LEN);

    int fd = open(path, flags, mode);

    node->resp.open.fd = fd;
}

void handle_polling_fs_read(struct dq_node *node)
{
    int fd = node->req.read.fd;
    size_t count = node->req.read.count;

    ssize_t ret = read(fd, node->resp.read.buf, count);

    node->resp.read.count = ret;
}

void handle_polling_fs_write(struct dq_node *node)
{
    int fd = node->req.write.fd;
    size_t count = node->req.write.count;

    ssize_t ret = write(fd, node->req.write.buf, count);

    node->resp.write.count = ret;
}

void handle_polling_fs_close(struct dq_node *node)
{
    int fd = node->req.close.fd;

    int ret = close(fd);

    node->resp.close.ret = ret;
}

void handle_polling_fs_empty(struct dq_node *node)
{
    /* no-op */
}

void handle_polling_kernel_flush_tlb(struct dq_node *node)
{
    u64 memcpy_src_pa = node->req.flush_tlb.memcpy_src_pa;
    u64 memcpy_dst_pa = node->req.flush_tlb.memcpy_dst_pa;
    u64 memcpy_len = node->req.flush_tlb.memcpy_len;
    u64 memcpy_fault_va = node->req.flush_tlb.memcpy_fault_va;
    u64 memcpy_vmspace = node->req.flush_tlb.memcpy_vmspace;

    int ret = usys_memcpy_and_flush_tlb(memcpy_src_pa,
                                        memcpy_dst_pa,
                                        memcpy_len,
                                        memcpy_fault_va,
                                        memcpy_vmspace);
    extern int my_id;
    assert(my_id >= 0);
    node->resp.flush_tlb.reply_result = ret;
    node->resp.flush_tlb.reply_from = my_id;
    node->resp.flush_tlb.reply_received = 1;
}

void handle_polling_print_debug_info(struct dq_node *node)
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
