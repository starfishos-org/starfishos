#pragma once

#include <common/types.h>
#include <common/lock.h>
#include <common/macro.h>
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

/*
 * ABI NOTE: every declaration in this file down to struct polling_shm_region
 * is shared, byte for byte, with user/system-servers/polling/polling.h.  The
 * kernel is a producer on the same lock-free queues that the user-space
 * polling server consumes, through CXL shared memory, so a field that changes
 * width or position on one side and not the other does not fail to build — it
 * silently makes the two sides read different bytes.  Change both together.
 *
 * Mutable queue roots and node links carry a generation counter beside the
 * offset: nodes are recycled, so a bare offset is not a safe CAS value (the
 * same offset can leave the structure and reappear while a producer still
 * holds an old snapshot).  Bump the generation on every successful store.
 */
typedef u64 qtagptr_t;

static inline qtagptr_t qtagptr_make(u32 generation, qptr_t off)
{
    return ((u64)generation << 32) | (u32)off;
}

static inline qptr_t qtagptr_offset(qtagptr_t ptr)
{
    return (qptr_t)(u32)ptr;
}

static inline qtagptr_t qtagptr_advance(qtagptr_t old, qptr_t off)
{
    return qtagptr_make((u32)(old >> 32) + 1, off);
}

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

/*
 * Mirrored byte-for-byte by user/system-servers/polling/polling.h: the queue
 * lives in CXL shared memory and both sides cast the same bytes.  Only ever
 * append here, and append to the user-side copy in the same order.
 *
 * POLLING_PRINT_DEBUG_INFO is produced only by userspace, but it must keep its
 * slot so the enumerator values on both sides agree.
 */
enum polling_request_type {
    POLLING_FS_REQ_OPEN,
    POLLING_FS_REQ_READ,
    POLLING_FS_REQ_WRITE,
    POLLING_FS_REQ_CLOSE,
    POLLING_REQ_EMPTY,
    POLLING_KERNEL_REQ_FLUSH_TLB,
    POLLING_PRINT_DEBUG_INFO,
    POLLING_KERNEL_REQ_FLUSH_TLB_BATCH,
    POLLING_KERNEL_REQ_CXL_DEMOTE_BATCH,
};

#define CXL_DEMOTE_WIRE_MAX_OPS 64

enum cxl_demote_wire_phase {
    CXL_DEMOTE_WIRE_FLUSH = 0,
    CXL_DEMOTE_WIRE_COPY,
    CXL_DEMOTE_WIRE_FREE_ORIGIN,
};

struct polling_cxl_demote_op {
    u64 src_pa;
    u64 dst_pa;
    u64 fault_va;
    u64 vmspace_ptr;
    u64 txn_id;
};

struct polling_fs_req_open {
    char path[FS_REQ_PATH_BUF_LEN];
    int flags;
    int mode;
};

/*
 * The kernel never produces a read request; it mirrors the user-side struct
 * only so that the byte-for-byte claim above stays literally true.  `offset`
 * is user-space `off_t`, i.e. a signed 64-bit value.
 */
struct polling_fs_req_read {
    int fd;
    size_t count;
    s64 offset;
    int positioned;
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

/*
 * Batched page migration.
 *
 * One page per request costs two all-CPU TLB shootdown IPIs on the remote
 * machine (see sys_memcpy_and_flush_tlb), which dwarfs the 4 KiB copy itself.
 * Carrying a run of pages in a single request amortizes both the RPC and those
 * two shootdowns over the whole batch.
 *
 * 32 entries is 792 bytes, well inside the union whose size is already set by
 * polling_fs_req_write::buf (PAGE_SIZE), so struct polling_request and the
 * queue node keep their current size and the shared-memory layout is unchanged.
 * The static assert below enforces that: growing the node would change
 * DQ_MAX_NODES, and a pool size mismatch between the two sides is exactly the
 * kind of silent cross-machine breakage the ABI note above describes.
 */
#define POLLING_TLB_BATCH_MAX 32

struct polling_tlb_batch_entry {
    u64 src_pa;
    u64 dst_pa;
    u64 fault_va;
};

struct polling_kernel_req_flush_tlb_batch {
    u64 memcpy_vmspace;
    u64 memcpy_len; /* per-entry length, always PAGE_SIZE */
    u64 count;
    struct polling_tlb_batch_entry entries[POLLING_TLB_BATCH_MAX];
};

struct polling_kernel_req_cxl_demote_batch {
    u32 phase;
    u32 count;
    struct polling_cxl_demote_op ops[CXL_DEMOTE_WIRE_MAX_OPS];
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
        struct polling_kernel_req_flush_tlb_batch flush_tlb_batch;
        struct polling_kernel_req_cxl_demote_batch cxl_demote;
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

struct polling_kernel_resp_cxl_demote_batch {
    s32 result;
};

struct polling_response {
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
        struct polling_resp_empty empty;
        struct polling_kernel_resp_flush_tlb flush_tlb;
        struct polling_kernel_resp_cxl_demote_batch cxl_demote;
    } __attribute__((aligned(8)));
};

/*
 * Queue Node (kernel side — non-atomic fields, kernel uses its own atomic ops).
 */
struct dq_node {
    qtagptr_t next; /* ABA-safe offset-based pointer */
    int status;     /* enum dq_status */
    struct polling_request req;
    struct polling_response resp;
};

/*
 * Durable Queue: { head, tail, queue_lock }
 */
struct durable_queue {
    qtagptr_t head;
    qtagptr_t tail;
    struct lock queue_lock;  /* Lock for enqueus/dequeue operations */
} __attribute__((aligned(64)));

/*
 * Node allocator (Treiber stack free list).
 */
struct dq_allocator {
    qtagptr_t free_list; /* ABA-safe Treiber stack head */
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

/*
 * Guard the layout the user-space polling server assumes.  These fired
 * nowhere when the server switched its queue links to tagged pointers and the
 * kernel did not: the kernel kept writing status/req at the old offsets, the
 * server kept reading the new ones, and cross-machine page migration hung with
 * no diagnostic at all.  Keep the two headers in lockstep.
 */
_Static_assert(sizeof(qtagptr_t) == 8, "queue link must stay 64-bit");
_Static_assert(offsetof(struct dq_node, status) == 8, "dq_node ABI drift");
_Static_assert(offsetof(struct dq_node, req) == 16, "dq_node ABI drift");
_Static_assert(sizeof(struct polling_kernel_req_flush_tlb_batch)
                       <= sizeof(struct polling_fs_req_write),
               "TLB batch request must not grow the queue node");
/*
 * The read request is produced only by user space, but it shares the request
 * union, so its layout still has to match: the same assertions exist in
 * user/system-servers/polling/polling.h.
 */
_Static_assert(offsetof(struct polling_fs_req_read, count) == 8,
               "polling_fs_req_read ABI drift");
_Static_assert(offsetof(struct polling_fs_req_read, offset) == 16,
               "polling_fs_req_read ABI drift");
_Static_assert(offsetof(struct polling_fs_req_read, positioned) == 24,
               "polling_fs_req_read ABI drift");
_Static_assert(offsetof(struct durable_queue, tail) == 8,
               "durable_queue ABI drift");
_Static_assert(sizeof(struct durable_queue) == 64, "durable_queue ABI drift");
_Static_assert(offsetof(struct dq_allocator, node_size) == 8,
               "dq_allocator ABI drift");
_Static_assert(sizeof(struct dq_allocator) == 64, "dq_allocator ABI drift");
_Static_assert(sizeof(struct polling_shm_region) == 128,
               "polling_shm_region ABI drift");

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
 * Scheduler and notification waiting lists.  These are kernel-to-kernel only,
 * never read by a user-space server, so they keep plain offsets and must not
 * be expressed with struct durable_queue: sharing that type once meant a
 * change made for the polling ABI silently rewrote the scheduler's queues too.
 */
struct thread_durable_queue {
    qptr_t head;
    qptr_t tail;
    struct lock queue_lock;
} __attribute__((aligned(64)));

/*
 * Pool of thread_dq_nodes, stored in CXL SHM.
 * Free list is a Treiber stack.
 */
struct thread_dq_pool {
    qptr_t free_list;   /* head of Treiber stack free list */
    s32 node_count;     /* total nodes allocated */
    /*
     * Publishes the pool and scheduler shared-queue sentinels as one unit.
     * Machine 0 is the sole initializer; other machines wait for READY.
     */
    volatile u32 init_state;
    char _pad[52];      /* padding to cache line */
    struct thread_dq_node nodes[THREAD_DQ_POOL_SIZE];
} __attribute__((aligned(64)));

enum thread_dq_pool_init_state {
    THREAD_DQ_POOL_UNINIT = 0,
    THREAD_DQ_POOL_INITIALIZING,
    THREAD_DQ_POOL_READY,
};

/* Helper functions for thread queue offset calculations - declared in shm.c */
void *thread_qptr_to_ptr(qptr_t off);
qptr_t thread_ptr_to_qptr(void *ptr);

/* ---- API ---- */

void shm_init(void);
int sys_mmap_shm(u32 shm_id, void *addr);

/* Kernel-side queue operations (used for TLB flush IPI via polling) */
struct dq_node *dq_alloc_node(struct polling_shm_region *shm);
struct dq_node *dq_alloc_node_timeout(struct polling_shm_region *shm,
                                      u64 timeout_ns);
void dq_enqueue(struct polling_shm_region *shm, struct dq_node *node,
                struct polling_request *req);
void dq_wait_for_done(struct dq_node *node);
void dq_mark_consumed(struct dq_node *node);
int dq_wait_for_done_timeout(struct dq_node *node, u64 timeout_ns);

/* Forward declaration for durable queue operations */
struct thread;

/* Kernel-side thread durable queue operations (for sched & notification) */
void thread_dq_pool_init(void);
int thread_dq_init(struct thread_durable_queue *q);
void thread_dq_enqueue(struct thread_durable_queue *q, struct thread *thread);
struct thread *thread_dq_dequeue(struct thread_durable_queue *q);
void thread_dq_cancel_node(qptr_t node_off);

#ifndef MAX_SHM_NUM
#define MAX_SHM_NUM (2 * CLUSTER_MAX_MACHINE_NUM)
#endif
