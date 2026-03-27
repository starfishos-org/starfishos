#pragma once

#include "polling_config.h"

#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fs_wrapper_defs.h>

/*
 * Durable Queue — lock-free MPSC queue with offset-based pointers.
 *
 * Matches the algorithm from docs/durable-queue.md:
 *   Node  { Node* next; status; Payload payload; }
 *   Queue { Node* head; Node* tail; }
 *
 * Since producer and consumer may map the SHM at different virtual
 * addresses, we use byte offsets from the SHM region base instead
 * of raw pointers. The allocator is a separate Treiber stack that
 * manages a pool of fixed-size nodes within the SHM.
 *
 * Status transitions:
 *   FREE (in allocator) -> INIT (enqueued) -> DOING (consumer claimed)
 *                       -> DONE (response ready) -> FREE (recycled)
 */

/* ---- Offset-based pointer (replaces raw Node*) ---- */

typedef int32_t qptr_t; /* byte offset from SHM base, -1 = NULL */
#define QPTR_NULL ((qptr_t)-1)

static inline void *qptr_to_ptr(void *shm_base, qptr_t off)
{
    return (off == QPTR_NULL) ? NULL : (char *)shm_base + off;
}

static inline qptr_t ptr_to_qptr(void *shm_base, void *ptr)
{
    return (ptr == NULL) ? QPTR_NULL : (qptr_t)((char *)ptr - (char *)shm_base);
}

/* ---- Persistence stub (no-op for DRAM/CXL) ---- */

#define FLUSH(addr) do { /* no-op */ } while (0)

/* ---- Node status ---- */

enum dq_status {
    DQ_FREE = 0,
    DQ_INIT,
    DQ_DOING,
    DQ_DONE,
    DQ_CRASH,
};

/* ---- Request / Response types (payload) ---- */

enum polling_request_type {
    POLLING_FS_REQ_OPEN,
    POLLING_FS_REQ_READ,
    POLLING_FS_REQ_WRITE,
    POLLING_FS_REQ_CLOSE,
    POLLING_REQ_EMPTY,
    POLLING_KERNEL_REQ_FLUSH_TLB,
    POLLING_PRINT_DEBUG_INFO,
};

struct polling_fs_req_open {
    char path[FS_REQ_PATH_BUF_LEN];
    int flags;
    int mode;
};

struct polling_fs_req_read {
    int fd;
    size_t count;
};

struct polling_fs_req_write {
    int fd;
    char buf[POLLING_FS_WRITE_BUF_SIZE];
    size_t count;
};

struct polling_fs_req_close {
    int fd;
};

struct polling_req_empty {};

struct polling_kernel_req_flush_tlb {
    u64 memcpy_src_pa;
    u64 memcpy_dst_pa;
    u64 memcpy_len;
    u64 memcpy_fault_va;
    u64 memcpy_vmspace;
};

struct memcpy_flush_tlb_op {
    u64 src_pa;
    u64 dst_pa;
    u64 len;
    u64 fault_va;
    u64 vmspace_ptr;
};

struct polling_req_print_debug_info {};

struct polling_request {
    enum polling_request_type type;
    union {
        struct polling_fs_req_open open;
        struct polling_fs_req_read read;
        struct polling_fs_req_write write;
        struct polling_fs_req_close close;
        struct polling_req_empty empty;
        struct polling_kernel_req_flush_tlb flush_tlb;
        struct polling_req_print_debug_info print_debug_info;
    } __attribute__((aligned(8)));
};

struct polling_fs_resp_open {
    int fd;
};

struct polling_fs_resp_read {
    ssize_t count;
    char buf[POLLING_FS_READ_BUF_SIZE];
};

struct polling_fs_resp_write {
    ssize_t count;
};

struct polling_fs_resp_close {
    int ret;
};

struct polling_resp_empty {};

struct polling_kernel_resp_flush_tlb {
    u32 reply_received;
    u32 reply_from;
    s32 reply_result;
};

struct polling_resp_print_debug_info {};

struct polling_response {
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
        struct polling_resp_empty empty;
        struct polling_kernel_resp_flush_tlb flush_tlb;
        struct polling_resp_print_debug_info print_debug_info;
    } __attribute__((aligned(8)));
};

/*
 * Queue Node.
 *
 * Doc: Node { Node* next; status; Payload payload; }
 *
 * The payload is a union: the producer writes the request, the consumer
 * reads it, processes, then writes the response into the same memory.
 * The producer spins on status == DQ_DONE, then reads the response.
 */
struct dq_node {
    _Atomic qptr_t next;   /* offset-based pointer to next node */
    _Atomic int status;    /* enum dq_status */
    struct polling_request req;
    struct polling_response resp;
};

/*
 * Durable Queue.
 *
 * Doc: Queue { Node* head; Node* tail; }
 */
struct durable_queue {
    _Atomic qptr_t head;
    _Atomic qptr_t tail;
} __attribute__((aligned(64)));

/*
 * Node allocator — Treiber stack free list, separate from the queue.
 * Manages a pool of fixed-size nodes within the SHM region.
 */
struct dq_allocator {
    _Atomic qptr_t free_list; /* Treiber stack head */
    int32_t node_size;        /* sizeof(dq_node), rounded up */
    int32_t node_count;       /* total nodes in pool */
    int32_t pool_offset;      /* byte offset of node pool from SHM base */
} __attribute__((aligned(64)));

/*
 * SHM region layout:
 *   [ durable_queue | dq_allocator | sentinel_node | node_pool... ]
 */
struct polling_shm_region {
    struct durable_queue queue;
    struct dq_allocator alloc;
    /* Node pool starts here; nodes are accessed via offsets. */
};

/* Compute the pool offset and max node count */
#define DQ_POOL_OFFSET \
    ((int32_t)sizeof(struct polling_shm_region))

#define DQ_NODE_SIZE \
    ((int32_t)((sizeof(struct dq_node) + 7) & ~7))

#define DQ_MAX_NODES \
    ((int32_t)((POLLING_SHM_SIZE - DQ_POOL_OFFSET) / DQ_NODE_SIZE))

static_assert(DQ_MAX_NODES >= 2,
              "SHM too small: need at least 2 nodes (1 sentinel + 1 data)");
