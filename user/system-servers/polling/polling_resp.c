#include "polling_resp.h"
#include "polling.h"
#include "polling_utils.h"
#include "polling_config.h"

#include <chcore/syscall.h>
#include <chcore/ipc.h>
#include <chcore-internal/fs_defs.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* batch IPC helpers */
extern ipc_struct_t *get_ipc_struct_by_mount_id(int mount_id);

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

/* Set to 1 to dump server-side timing data in polling_print_debug_info() */
#define ENABLE_SRV_TIMING 0

void handle_polling_print_debug_info(struct dq_node *node)
{
#if ENABLE_SRV_TIMING
    /* Dump server-side dequeue + handle timing */
    extern void srv_dump_timing(void);
    srv_dump_timing();
#endif
}

/*
 * Batch read: send N read requests to tmpfs in a single IPC call.
 *
 * All nodes in batch[] must be POLLING_FS_REQ_READ.
 * We extract fd/count from each node, build an FS_REQ_BATCH_READ IPC message,
 * do one ipc_call, then scatter the results back to each node's response.
 */
void handle_batch_reads(struct dq_node **batch, int count)
{
    if (count <= 0) return;

    /* Use the first fd to get the IPC connection to tmpfs */
    int first_fd = batch[0]->req.read.fd;
    int mount_id = chcore_get_mount_id(first_fd);
    if (mount_id < 0) {
        /* Fallback: handle individually */
        for (int i = 0; i < count; i++)
            handle_polling_fs_read(batch[i]);
        return;
    }

    ipc_struct_t *fs_ipc = get_ipc_struct_by_mount_id(mount_id);
    if (!fs_ipc) {
        for (int i = 0; i < count; i++)
            handle_polling_fs_read(batch[i]);
        return;
    }

    /* Build batch IPC message */
    ipc_msg_t *ipc_msg = ipc_create_msg(fs_ipc, IPC_SHM_AVAILABLE, 0);
    char *buf = ipc_get_msg_data(ipc_msg);

    struct fs_batch_read_header *hdr = (struct fs_batch_read_header *)buf;
    hdr->req = FS_REQ_BATCH_READ;
    hdr->count = count;

    struct fs_batch_read_entry *entries =
        (struct fs_batch_read_entry *)(hdr + 1);
    for (int i = 0; i < count; i++) {
        entries[i].fd = batch[i]->req.read.fd;
        entries[i].count = (int)batch[i]->req.read.count;
    }

    /* Single IPC call — one context switch to tmpfs and back */
    ssize_t total = ipc_call(fs_ipc, ipc_msg);

    /* Parse response: ret_arr[count] then concatenated data */
    ssize_t *ret_arr = (ssize_t *)buf;
    char *data_ptr = buf + count * sizeof(ssize_t);

    for (int i = 0; i < count; i++) {
        ssize_t n = ret_arr[i];
        if (n > 0) {
            memcpy(batch[i]->resp.read.buf, data_ptr, n);
            data_ptr += n;
        }
        batch[i]->resp.read.count = n;
    }

    ipc_destroy_msg(ipc_msg);
    (void)total;
}
