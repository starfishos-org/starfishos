#pragma once

#include <common/types.h>
#include <common/lock.h>
#include <mm/mm.h>
#include <posix/sys/types.h>

/*
 * Kernel-side mirror of user/system-servers/polling/polling.h
 *
 * Durable Queue with offset-based pointers for cross-address-space safety.
 * Matches the algorithm from docs/durable-queue.md.
 */

#define POLLING_SHM_SIZE (PAGE_SIZE * 64UL)

/* P-log SHM for Ananke-style FS recovery */
#define PLOG_SHM_SIZE     (PAGE_SIZE * 1024UL) /* 4MB per p-log */
#define PLOG_SHM_ID_BASE  CLUSTER_MAX_MACHINE_NUM
#define PLOG_SHM_ID(mid)  (PLOG_SHM_ID_BASE + (mid))

/* Dedicated persistent CXLFS device: one fixed 1 GiB slice per machine. */
#define CXLFS_SHM_SIZE     (1UL << 30)
#define CXLFS_SHM_ID_BASE  (2 * CLUSTER_MAX_MACHINE_NUM)
#define CXLFS_SHM_ID(mid)  (CXLFS_SHM_ID_BASE + (mid))
#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)
#define FS_REQ_PATH_BUF_LEN 256

/* ---- Offset-based pointer ---- */

typedef s32 qptr_t;
#define QPTR_NULL ((qptr_t)-1)

/* FLUSH: use common/mem_sync.h (clwb) in kernel code */

/* ---- Node status ---- */

enum dq_status {
    DQ_FREE = 0,
    DQ_INIT,
    DQ_DOING,
    DQ_DONE,
    DQ_CONSUMED,
    DQ_CRASH,
    DQ_CANCELLED,
};

/* ---- Request / Response types ---- */

enum polling_request_type {
    POLLING_FS_REQ_OPEN,
    POLLING_FS_REQ_READ,
    POLLING_FS_REQ_WRITE,
    POLLING_FS_REQ_CLOSE,
    POLLING_REQ_EMPTY,
    POLLING_KERNEL_REQ_FLUSH_TLB,
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

struct polling_request {
    enum polling_request_type type;
    union {
        struct polling_fs_req_open open;
        struct polling_fs_req_read read;
        struct polling_fs_req_write write;
        struct polling_fs_req_close close;
        struct polling_req_empty empty;
        struct polling_kernel_req_flush_tlb flush_tlb;
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

struct polling_response {
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
        struct polling_resp_empty empty;
        struct polling_kernel_resp_flush_tlb flush_tlb;
    } __attribute__((aligned(8)));
};

/*
 * Queue Node (kernel side — non-atomic fields, kernel uses its own atomic ops).
 */
struct dq_node {
    qptr_t next;   /* offset-based pointer */
    int status;    /* enum dq_status */
    struct polling_request req;
    struct polling_response resp;
};

/*
 * Durable Queue: { head, tail, queue_lock }
 */
struct durable_queue {
    qptr_t head;
    qptr_t tail;
    struct lock queue_lock;  /* Lock for enqueus/dequeue operations */
} __attribute__((aligned(64)));

/*
 * Node allocator (Treiber stack free list).
 */
struct dq_allocator {
    qptr_t free_list;
    s32 node_size;
    s32 node_count;
    s32 pool_offset;
} __attribute__((aligned(64)));

/*
 * SHM region.
 */
struct polling_shm_region {
    struct durable_queue queue;
    struct dq_allocator alloc;
    /* node pool follows */
};

#define DQ_POOL_OFFSET \
    ((s32)sizeof(struct polling_shm_region))

#define DQ_NODE_SIZE \
    ((s32)((sizeof(struct dq_node) + 7) & ~7))

#define DQ_MAX_NODES \
    ((s32)((POLLING_SHM_SIZE - DQ_POOL_OFFSET) / DQ_NODE_SIZE))

/* Offset helpers (kernel side) */
static inline void *qptr_to_ptr(void *shm_base, qptr_t off)
{
    return (off == QPTR_NULL) ? NULL : (char *)shm_base + off;
}

static inline qptr_t ptr_to_qptr(void *shm_base, void *ptr)
{
    return (ptr == NULL) ? QPTR_NULL : (qptr_t)((char *)ptr - (char *)shm_base);
}

/* ---- Thread Durable Queue (for scheduler & notification) ---- */

#define THREAD_DQ_POOL_SIZE 4096

/*
 * Node for thread durable queue.
 * Stores: next pointer, status, and thread physical address.
 */
struct thread_dq_node {
    qptr_t next;        /* offset-based pointer to next node */
    int status;         /* enum dq_status */
    u64 thread_pa;      /* physical address of the thread struct */
} __attribute__((aligned(16)));

/*
 * Pool of thread_dq_nodes, stored in CXL SHM.
 * Free list is a Treiber stack.
 */
struct thread_dq_pool {
    qptr_t free_list;   /* head of Treiber stack free list */
    s32 node_count;     /* total nodes allocated */
    char _pad[56];      /* padding to cache line */
    struct thread_dq_node nodes[THREAD_DQ_POOL_SIZE];
} __attribute__((aligned(64)));

/* Helper functions for thread queue offset calculations - declared in shm.c */
void *thread_qptr_to_ptr(qptr_t off);
qptr_t thread_ptr_to_qptr(void *ptr);

/* ---- API ---- */

void shm_init(void);
int sys_mmap_shm(u32 shm_id, void *addr);

/* Kernel-side queue operations (used for TLB flush IPI via polling) */
struct dq_node *dq_alloc_node(struct polling_shm_region *shm);
void dq_enqueue(struct polling_shm_region *shm, struct dq_node *node,
                struct polling_request *req);
void dq_wait_for_done(struct dq_node *node);

/* Forward declaration for durable queue operations */
struct thread;
struct durable_queue;

/* Kernel-side thread durable queue operations (for sched & notification) */
void thread_dq_pool_init(void);
int thread_dq_init(struct durable_queue *q);
void thread_dq_enqueue(struct durable_queue *q, struct thread *thread);
struct thread *thread_dq_dequeue(struct durable_queue *q);
void thread_dq_cancel_node(qptr_t node_off);

#ifndef MAX_SHM_NUM
#define MAX_SHM_NUM (2 * CLUSTER_MAX_MACHINE_NUM)
#endif
