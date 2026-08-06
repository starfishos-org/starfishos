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
#include <stddef.h>
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

/*
 * Queue nodes are recycled, so an offset alone is not a safe CAS value: the
 * same offset can leave a lock-free data structure and later reappear while a
 * producer still holds an old snapshot.  Pair mutable roots with a generation
 * counter to reject those ABA snapshots.  Node links are tagged too: a
 * producer can pause after validating the tail while that node is reclaimed
 * and reused, so protecting only the queue roots is insufficient.
 */
typedef uint64_t qtagptr_t;

static inline qtagptr_t qtagptr_make(uint32_t generation, qptr_t off)
{
    return ((uint64_t)generation << 32) | (uint32_t)off;
}

static inline qptr_t qtagptr_offset(qtagptr_t ptr)
{
    return (qptr_t)(uint32_t)ptr;
}

static inline qtagptr_t qtagptr_advance(qtagptr_t old, qptr_t off)
{
    return qtagptr_make((uint32_t)(old >> 32) + 1, off);
}

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
    DQ_CONSUMED, /* producer finished reading response */
    DQ_CRASH,
};

/* ---- Request / Response types (payload) ---- */

/*
 * Mirrors kernel/include/mm/shm.h byte for byte — the queue lives in CXL
 * shared memory and both sides cast the same bytes.  Only ever append, and
 * append to the kernel copy in the same order.
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

struct polling_fs_req_read {
    int fd;
    size_t count;
    off_t offset;
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

struct polling_kernel_req_cxl_demote_batch {
    u32 phase;
    u32 count;
    struct polling_cxl_demote_op ops[CXL_DEMOTE_WIRE_MAX_OPS];
};

struct memcpy_flush_tlb_op {
    u64 src_pa;
    u64 dst_pa;
    u64 len;
    u64 fault_va;
    u64 vmspace_ptr;
};

/*
 * Batched page migration — see kernel/include/mm/shm.h for the rationale.
 * 32 entries is 792 bytes, inside the union size already set by
 * polling_fs_req_write::buf, so the node layout is unchanged.  The static
 * assert further down enforces that; growing the node would change
 * DQ_MAX_NODES and silently desynchronize the two sides.
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
        struct polling_kernel_resp_cxl_demote_batch cxl_demote;
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
    _Atomic qtagptr_t next; /* ABA-safe offset-based pointer to next node */
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
    _Atomic qtagptr_t head;
    _Atomic qtagptr_t tail;
} __attribute__((aligned(64)));

/*
 * Node allocator — Treiber stack free list, separate from the queue.
 * Manages a pool of fixed-size nodes within the SHM region.
 */
struct dq_allocator {
    _Atomic qtagptr_t free_list; /* ABA-safe Treiber stack head */
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

/*
 * Must match the kernel-side assert in kernel/include/mm/shm.h: a batch
 * request that outgrows polling_fs_req_write would enlarge dq_node, change
 * DQ_MAX_NODES, and desynchronize the shared node pool between the two sides.
 */
static_assert(sizeof(struct polling_kernel_req_flush_tlb_batch)
                      <= sizeof(struct polling_fs_req_write),
              "TLB batch request must not grow the queue node");

/*
 * The read request is produced only here, but it shares the request union with
 * the requests the kernel produces, so its layout is still part of the shared
 * ABI. The same assertions exist in kernel/include/mm/shm.h.
 */
static_assert(offsetof(struct polling_fs_req_read, count) == 8,
              "polling_fs_req_read ABI drift");
static_assert(offsetof(struct polling_fs_req_read, offset) == 16,
              "polling_fs_req_read ABI drift");
static_assert(offsetof(struct polling_fs_req_read, positioned) == 24,
              "polling_fs_req_read ABI drift");
