#pragma once

#include <dsm/dsm_ref.h>
#include <mm/vmspace.h>
#include <sched/sched.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <common/util.h>
#include <mm/mm.h>
#include <mm/slab.h>
#include <mm/shm.h>
#include <machine.h>
#include <uapi/types.h>

// #define DSM_DEBUG

#define DSM_PREFIX "[DSM]"
#define CXL_RECENT_DEMOTE_SLOTS 256

#define dsm_info(fmt, ...)  printk(DSM_PREFIX " " fmt, ##__VA_ARGS__)
#define dsm_error(fmt, ...) printk(DSM_PREFIX " " fmt, ##__VA_ARGS__)
#ifdef DSM_DEBUG
#define dsm_debug(fmt, ...) printk(DSM_PREFIX " " fmt, ##__VA_ARGS__)
#else
#define dsm_debug(fmt, ...)
#endif

// #define MULTI_PT_DEBUG

#ifdef MULTI_PT_DEBUG
#define multipt_debug(fmt, ...) printk("[MULTI_PT] " fmt, ##__VA_ARGS__)
#else
#define multipt_debug(fmt, ...)
#endif

#define pingpong_info(thread, fmt, ...)                                      \
    if (!strcmp(thread->cap_group->cap_group_name, "/pingpong-pthread.bin")) \
    printk("[pingpong] " fmt, ##__VA_ARGS__)

/**
 * Fast access to dsm metadata
 */
#define DSM_CONFIG_STATE_UNINITED 0
#define DSM_CONFIG_STATE_MM_INITED 1
#define DSM_CONFIG_STATE_CKPT_INITED 2
#define DSM_STATE (dsm_meta->state)

/**
 * cluster machine num
 * when register every machine, it will increase (LOCAL CPU NUM)
 */
#define CLUSTER_MAX_CPU_NUM (1024)
#define CLUSTER_CPU_NUM     (dsm_meta->cluster_cpu_num)

#ifndef CLUSTER_MAX_MACHINE_NUM
#define CLUSTER_MAX_MACHINE_NUM (8)
#endif

#ifndef DSM_FIXED_MACHINE_NUM
#define DSM_FIXED_MACHINE_NUM (4)
#endif
#ifndef CLUSTER_MACHINE_NUM
#define CLUSTER_MACHINE_NUM     (dsm_meta->cluster_machine_num)
#endif

#ifndef MAX_SHM_NUM
#define MAX_SHM_NUM (2 * CLUSTER_MAX_MACHINE_NUM)
#endif

/**
 * machine ID of current machine
 */
extern mid_t machine_id;
#define CUR_MACHINE_ID (machine_id)

/**
 * cpu range of current machine
 */
u32 cpu_range_low, cpu_range_high;

#define CPU_RANGE_LOW  (cpu_range_low)
#define CPU_RANGE_HIGH (cpu_range_high)

/* local to global, global to local */
#ifndef cpuid_l2g
#define cpuid_l2g(x) ((x) + CPU_RANGE_LOW)
#endif
#ifndef cpuid_g2l
#define cpuid_g2l(x) ((x) - CPU_RANGE_LOW)
#endif

static bool inline is_local_cpu(u32 cpuid)
{
    return ((cpuid <= CPU_RANGE_HIGH) && (cpuid >= CPU_RANGE_LOW))
           || cpuid == NO_AFF;
}

typedef struct {
    u32 cpu_range_low;
    u32 cpu_range_high;
    u64 local_mem_start;
    u64 local_mem_size;
} dsm_machine_local_metadata_t;

/**
 * A batch of physical addresses handed back to the machine that owns them.
 * The node lives in CXL so that both machines can reach it; the pages it
 * names do not, which is why they travel as plain addresses.
 *
 * Sized to fill one 2 KiB slab object.
 */
#define DRAM_PAGE_FREE_BATCH_MAX (254)

struct dram_page_free_batch {
    /*
     * Written by the producer, followed by the owner.  A plain pointer is
     * portable because every machine maps CXL SHM at the same kernel VA --
     * the same assumption that lets dsm_meta itself be a pointer.
     */
    struct dram_page_free_batch *next;
    u32 count;
    u32 owner;
    paddr_t pas[DRAM_PAGE_FREE_BATCH_MAX];
};

/* One max-order slab object; anything larger takes a whole page instead. */
_Static_assert(sizeof(struct dram_page_free_batch)
                       == (1UL << SLAB_MAX_ORDER),
               "dram_page_free_batch should fill exactly one slab object");

/**
 * MSI message types for inter-machine communication
 */
enum msi_msg_type {
    MSI_MSG_TYPE_TLB_FLUSH = 0,  /* TLB flush request */
    MSI_MSG_TYPE_TEST = 1,        /* Test message */
    MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB = 2,  /* Memcpy and flush TLB request */
    MSI_MSG_TYPE_CXL_DEMOTE_BATCH = 3,
    MSI_MSG_TYPE_MAX
};

/**
 * dsm metadata
 *
 * The main structure of dsm metadata.
 * It is shared among all machines, and is placed in the
 * first page of the shared memory so that it can be accessed
 * by all machines.
 *
 * The metadata contains several parts:
 * 1. global configuration
 * 2. dsm memory layout
 * 3. buddy and slab system
 * 4. shared queue
 * 5. checkpoint data
 */
typedef struct {
    /* magic number */
    char magic[8]; // "cxlmem" or "hostfs"

    /**
     * 1. global configuration
     */
    u32 cluster_cpu_num; // number of CPUs in the cluster
    u32 cluster_machine_num; // number of machines in the cluster
    volatile u64 state; // state of dsm

    /**
     * 2. dsm memory layout:
     * a shared and single virtual kernel space:
     *
     * local_paddr                                 shm_paddr + shm_size
     *    v
     *    || M1 LOCAL MEM || ... || Mn LOCAL MEM || SHM ||
     */
    u64 shm_paddr;
    u64 shm_size;
    u64 local_paddr;
    u64 max_paddr; // vaddr of max local DRAM

    /**
     * 3. local mem kernel addr of each machine
     */
    dsm_machine_local_metadata_t local_meta[CLUSTER_MAX_MACHINE_NUM];

    /**
     * 4. buddy and slab system of SHM
     */
    struct phys_mem_pool mem_pool[N_PHYS_MEM_POOLS];     // buddy system
    struct slab_pointer slab_pool[SLAB_MAX_ORDER + 1];   // slab system
    struct lock slabs_locks[SLAB_MAX_ORDER + 1];         // slab lock

#ifdef SLAB_CRASH_RECOVERY
    /**
     * 4b. Per-machine CXL slab pools and crash recovery logs.
     * Stored in CXL so they survive machine crashes.
     */
    struct {
        struct slab_pointer pool[PLAT_CPU_NUM][SLAB_MAX_ORDER + 1];
        struct lock         locks[PLAT_CPU_NUM][SLAB_MAX_ORDER + 1];
        struct slab_cpu_log cpu_logs[PLAT_CPU_NUM];
    } cxl_slab_meta[CLUSTER_MAX_MACHINE_NUM];
#endif

    /**
     * 4c. Per-machine deferred remote-free stacks for the CXL slab.
     * The per-CPU CXL slab pools/locks are machine-local (DRAM), so a
     * machine must never mutate another machine's slab lists. A machine
     * freeing an object whose slab is owned by another machine pushes it
     * onto the owner's stack here (the link is stored in the freed slot
     * itself). A shared per-owner lock protects detach versus push: a raw
     * Treiber head is not sufficient because a detached slab slot may be
     * returned and reused while another machine still holds the old head,
     * creating an ABA self-cycle. The owner drains its stack on its own
     * alloc/free path. Each stack sits on its own cache line.
     */
    struct {
        struct lock lock;
        void *volatile head;
        char pad[48];
    } cxl_slab_remote_free[CLUSTER_MAX_MACHINE_NUM]
        __attribute__((aligned(CACHELINE_SZ)));

    /* Global CLOCK queue and accounting for DRAM-backed pages resident in CXL. */
    struct {
        /*
         * CLOCK membership and per-page reclaim state. Keep this lock on a
         * cache line of its own: candidate selection may hold it while walking
         * shared page metadata, whereas DRAM-to-CXL admission only needs the
         * accounting lock below.
         */
        struct {
            struct lock lock;
            char pad[56];
        } fifo_guard __attribute__((aligned(CACHELINE_SZ)));
        struct list_head fifo;
        /*
         * Number of FIFO entries currently in CXL_RECLAIM_FREE_PENDING.
         * retry_pending_frees() has to walk the FIFO under the cluster-wide
         * FIFO lock above, so it must not walk it at all while this is zero.
         */
        volatile u64 free_pending_pages;
        u64 next_sequence;
        volatile u64 next_rpc_id;
        u32 reclaiming;
        u32 snapshotting;
        u32 initialized;

        /*
         * Capacity admission and aggregate counters.  This lock is separate
         * from the FIFO so a bounded candidate scan cannot stall a new
         * DRAM-to-CXL reservation.  The configured residency limit is a soft
         * reclaim threshold: admission remains allowed above it.
         */
        struct {
            struct lock lock;
            char pad[56];
        } account_guard __attribute__((aligned(CACHELINE_SZ)));
        volatile u64 total_pages;
        volatile u64 limit_pages;
        volatile u64 allocated_pages;
        volatile u64 reserved_pages;
        volatile u64 resident_pages;
        volatile u64 resident_reserved_pages;
        volatile u64 reclaimed_pages;
        volatile u64 soft_limit_overcommits;
        volatile u64 async_reclaim_requests;
        volatile u64 fault_fallbacks;
        volatile u64 admission_reported_misses;
        volatile u64 admission_reported_fallbacks;
        /* Aggregate CLOCK observability; updated atomically by the singleton. */
        volatile u64 clock_scans;
        volatile u64 clock_second_chances;
        volatile u64 clock_cold_evictions;
        volatile u64 clock_pressure_evictions;
        volatile u64 clock_scan_skips;
        volatile u64 clock_cooldown_skips;
        volatile u64 clock_stable_cold;
        volatile u64 promotion_pages;
        volatile u64 refault_pages;
        volatile u64 demote_conflicts;
        volatile u64 demote_transactions;
        volatile u64 demote_phase_prepare_ns;
        volatile u64 demote_phase_flush_ns;
        volatile u64 demote_phase_copy_ns;
        volatile u64 demote_phase_bitmap_ns;
        volatile u64 demote_phase_finish_ns;
        volatile u64 demote_phase_total_ns;
        volatile u64 demote_phase_max_ns;
        volatile u64 next_scan_ns;
        volatile u64 next_demote_ns;
        volatile u64 next_pressure_ns;
        u64 recent_demote_pa[CXL_RECENT_DEMOTE_SLOTS];
        /*
         * Coalesced headroom requested by over-limit admissions.
         * The background worker treats this as a level-triggered wake-up and
         * clears it once enough headroom exists.
         */
        volatile u64 pending_reclaim_pages;
    } cxl_reclaim __attribute__((aligned(CACHELINE_SZ)));

    /**
     * 4d. Per-machine deferred remote-free stacks for local DRAM pages.
     * A cross-machine process's anonymous PMO is backed by whichever
     * machine first faulted on each page, so its radix tree holds pages
     * from several machines' private DRAM. Only the owner has a struct
     * page for those (global_dram_mem[] covers one machine's slice), so
     * another machine cannot free them, and unlike the slab case above it
     * cannot thread the list through the freed memory either: that DRAM is
     * not addressable from here at all. It instead batches the physical
     * addresses into nodes allocated from CXL and pushes those onto the
     * owner's stack (lock-free Treiber stack); the owner drains it from
     * its own DRAM allocation path. Each head sits on its own cache line.
     *
     * The head is a tagged pointer, not a bare one: nodes are freed by the
     * owner back into the same CXL slab that producers allocate from, so a
     * node can leave the stack and reappear at the same address while a
     * producer still holds an old snapshot of the head. The generation in
     * the high bits makes that producer's compare-and-swap fail instead of
     * silently splicing a live chain. See kernel/mm/remote_free.c for the
     * encoding.
     */
    struct {
        volatile u64 head;
        char pad[56];
    } dram_page_remote_free[CLUSTER_MAX_MACHINE_NUM];

    /**
     * 5. shared queue for scheduler (using durable_queue structure)
     */
    struct thread_durable_queue shared_queue[CLUSTER_MAX_CPU_NUM];

    /**
     * 5b. thread durable queue pool (for scheduler & notification)
     */
    struct thread_dq_pool thread_dq_pool;

    /**
     * MSI message area for inter-machine communication
     * Each machine has a message slot and reply slot
     */
    struct lock msi_rpc_lock;
    struct {
        struct lock msg_lock;       /* Lock protecting this message slot */
        volatile u32 msg_from;      /* Source machine ID */
        volatile u32 msg_type;      /* Message type (see MSI_MSG_TYPE_*) */
        volatile u32 reply_received; /* Reply received flag */
        volatile u32 reply_from;    /* Reply from machine ID */
        /* Message-specific data (union-like usage based on msg_type) */
        /* For MSI_MSG_TYPE_TLB_FLUSH: */
        volatile u64 tlb_start_va;  /* TLB flush start address */
        volatile u64 tlb_len;       /* TLB flush length */
        volatile u64 tlb_vmspace;   /* TLB flush vmspace pointer */
        /* For MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB: */
        volatile u64 memcpy_src_pa;  /* Source physical address for memcpy */
        volatile u64 memcpy_dst_pa;  /* Destination physical address for memcpy */
        volatile u64 memcpy_len;     /* Length for memcpy */
        volatile u64 memcpy_fault_va; /* Fault virtual address (for TLB flush) */
        volatile u64 memcpy_vmspace;  /* vmspace pointer (for TLB flush) */
        /* For MSI_MSG_TYPE_CXL_DEMOTE_BATCH: */
        volatile u32 cxl_batch_phase;
        volatile u32 cxl_batch_count;
        volatile s32 cxl_batch_result;
        volatile u64 cxl_batch_rpc_id;
        volatile u64 cxl_batch_reply_rpc_id;
        struct {
            volatile u64 src_pa;
            volatile u64 dst_pa;
            volatile u64 fault_va;
            volatile u64 vmspace_ptr;
            volatile u64 txn_id;
        } cxl_batch_ops[CXL_DEMOTE_WIRE_MAX_OPS];
    } msi_test_msg[CLUSTER_MAX_MACHINE_NUM];

    /*
     * Demotion control has its own cluster-serialized mailbox.  It must not
     * share the legacy MSI request/reply slots: foreground TLB traffic also
     * writes those slots and can otherwise overwrite an in-flight reclaim
     * phase.  A polling-server reclaim pthread consumes the target slot, so
     * no VM-space operation runs from interrupt context and no foreground
     * durable-queue reader is occupied.
     */
    struct lock cxl_control_rpc_lock;
    struct {
        struct lock lock;
        volatile u32 pending;
        volatile u32 sender;
        volatile u32 phase;
        volatile u32 count;
        volatile s32 result;
        volatile u64 rpc_id;
        volatile u64 reply_rpc_id;
        struct {
            volatile u64 src_pa;
            volatile u64 dst_pa;
            volatile u64 fault_va;
            volatile u64 vmspace_ptr;
            volatile u64 txn_id;
        } ops[CXL_DEMOTE_WIRE_MAX_OPS];
    } cxl_control[CLUSTER_MAX_MACHINE_NUM];

    /* One-way ivshmem MSI delivery benchmark.  The sender publishes a request
     * in the target slot; the target MSI handler completes the sender slot. */
    struct {
        volatile u64 request_seq;
        volatile u64 handled_seq;
        volatile u64 completed_seq;
        volatile u32 sender_machine;
    } msi_bench[CLUSTER_MAX_MACHINE_NUM];
    
    /**
     * Mapping from machine_id to ivshmem peer_id
     * peer_id is assigned by ivshmem-server and may differ from machine_id
     * 0xFFFFFFFF means uninitialized
     */
    volatile u32 machine_to_peer_id[CLUSTER_MAX_MACHINE_NUM];

    /**
     * Doorbell registers for MSI notification (software-based)
     * Since ivshmem-plain doesn't support MSI-X, we implement doorbell in shared memory
     * Each machine has a doorbell register at offset (machine_id * sizeof(u32))
     */
    volatile u32 doorbell_regs[CLUSTER_MAX_MACHINE_NUM];

    struct {
        struct cap_group *root_cap_group;
        struct thread *procmgr_thread;
        struct thread *fsm_thread;
        struct thread *lwip_thread;
    } local_service_table[CLUSTER_MAX_MACHINE_NUM];

    /**
     * 6. for fsm
     */
    struct thread *tmpfs_thread[CLUSTER_MAX_MACHINE_NUM];

    /**
     * 7. checkpoint data
     */
#if defined CHCORE_SSI_SLS
    /* crash_last_time = 1 means unexpected */
    bool crash_last_time;
    /* Checkpoint time stamp */
    u64 version_number;
    /* Is doing ckpt (or else is restore) */
    bool ckpt_initialized;
    /* Checkpoint data */
    struct ckpt_ws_table *ckpt_whole_sys_table;
    /* A KVS to accelerate the lookup of ckpt cap_group */
    struct kvs *ckpt_cg_kvs;
#endif

    struct shm_data_t {
        struct pmobject *pmo;
        char *data;
    } shm_data[MAX_SHM_NUM];

    /**
     * 10. Per-machine redo-log + era for partial-failure-resilient ref counting.
     *     era matrix: DSM_REF_MAX_MACHINES^2 × 4 bytes = 256 B
     *     machine state: DSM_REF_MAX_MACHINES × (64 + 8×64) = 4608 B
     *     Total: ~4.8 KB
     */
    dsm_ref_meta_t ref_meta;

#ifdef PHOENIX_SCHED_TIMING
    /**
     * Cross-machine TSC calibration.
     * Each machine signals ready, all spin until every slot is ready,
     * then writes get_cycles() simultaneously.  After the barrier,
     * TSC_TO_M0(t) normalises any machine's raw TSC to machine-0's domain.
     */
    volatile u8  tsc_sync_ready[CLUSTER_MAX_MACHINE_NUM];
    volatile u64 tsc_sync[CLUSTER_MAX_MACHINE_NUM];
#endif
} __attribute__((aligned(SIZE_4K))) dsm_metadata_t;

dsm_metadata_t *dsm_meta;

/* local meta of current machine */
// #define local_meta (dsm_meta->local_meta[CUR_MACHINE_ID]);

static inline void dsm_init_meta(vaddr_t shm_vaddr)
{
    dsm_meta = (dsm_metadata_t *)shm_vaddr;
}

#ifdef PHOENIX_SCHED_TIMING
/* Convert a raw get_cycles() reading on the current machine to the
 * machine-0 TSC domain.  Valid only after dsm_tsc_sync_barrier(). */
static inline u64 dsm_tsc_to_m0(u64 local_tsc)
{
    return local_tsc
           - dsm_meta->tsc_sync[CUR_MACHINE_ID]
           + dsm_meta->tsc_sync[0];
}
#endif

static inline u64 dsm_is_inited()
{
    BUG_ON(!dsm_meta);
    return (DSM_STATE > DSM_CONFIG_STATE_UNINITED);
}

static inline void dsm_init_mm(paddr_t shm_paddr, size_t shm_size,
                               paddr_t local_paddr)
{
#ifdef DSM_CLEAR_FIRST
    memset((void *)phys_to_virt(shm_paddr), 0, sizeof(dsm_metadata_t));
#endif
    /* check and init shm_vaddr */
    if (dsm_meta->shm_paddr) {
        /* TODO: should remap shm */
        if (dsm_meta->shm_paddr != shm_paddr) {
            kwarn("[DSM] shm paddr mismatch, expect: %llu, get: %llu\n",
                  shm_paddr,
                  dsm_meta->shm_paddr);
        }
    } else {
        dsm_meta->shm_paddr = shm_paddr;
        dsm_meta->shm_size = shm_size;
        dsm_meta->local_paddr = local_paddr;
        dsm_meta->max_paddr = local_paddr;
    }
}

#define IS_SHM_PADDR(paddr) ( \
    (u64)paddr >= (u64)dsm_meta->shm_paddr \
    && (u64)paddr < (u64)dsm_meta->shm_paddr + (u64)dsm_meta->shm_size)
#define IS_LOCAL_PADDR(paddr, machineid) ( \
    (u64)paddr >= (u64)dsm_meta->local_meta[machineid].local_mem_start \
    && (u64)paddr < (u64)dsm_meta->local_meta[machineid].local_mem_start + \
    (u64)dsm_meta->local_meta[machineid].local_mem_size)
#define IS_INVALID_PADDR(paddr) ( \
    !(IS_SHM_PADDR(paddr) || IS_LOCAL_PADDR(paddr, CUR_MACHINE_ID)))

void dsm_add_machine(void);

static int inline cpuid_g2mid(u32 gcpuid)
{
    for (int i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (gcpuid >= dsm_meta->local_meta[i].cpu_range_low
            && gcpuid <= dsm_meta->local_meta[i].cpu_range_high)
            return i;
    }
    return -1;
}

static int inline cpuid_l2g_with_mid(u32 lcpuid, u32 mid)
{
    int gcpuid = -1;
    if (lcpuid == NO_AFF) {
        gcpuid = dsm_meta->local_meta[mid].cpu_range_low;
    } else {
        gcpuid = dsm_meta->local_meta[mid].cpu_range_low + lcpuid;
        BUG_ON(gcpuid > dsm_meta->local_meta[mid].cpu_range_high);
    }
    return gcpuid;
}
