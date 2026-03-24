#pragma once

#include <mm/vmspace.h>
#include <sched/sched.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <common/util.h>
#include <mm/mm.h>
#include <mm/slab.h>
#include <machine.h>
#include <uapi/types.h>

// #define DSM_DEBUG

#define DSM_PREFIX "[DSM]"

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
#define MAX_SHM_NUM (8)
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

struct shared_queue_meta {
    struct list_head queue_head;
    u32 queue_len;
    struct lock queue_lock;
};

typedef struct {
    u32 cpu_range_low;
    u32 cpu_range_high;
    u64 local_mem_start;
    u64 local_mem_size;
} dsm_machine_local_metadata_t;

/**
 * MSI message types for inter-machine communication
 */
enum msi_msg_type {
    MSI_MSG_TYPE_TLB_FLUSH = 0,  /* TLB flush request */
    MSI_MSG_TYPE_TEST = 1,        /* Test message */
    MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB = 2,  /* Memcpy and flush TLB request */
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

    /**
     * 5. shared queue for scheduler
     */
    struct shared_queue_meta shared_queue[CLUSTER_MAX_CPU_NUM];

    /**
     * MSI message area for inter-machine communication
     * Each machine has a message slot and reply slot
     */
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
    } msi_test_msg[CLUSTER_MAX_MACHINE_NUM];
    
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
     // for testing
} __attribute__((aligned(SIZE_4K))) dsm_metadata_t;

dsm_metadata_t *dsm_meta;

/* local meta of current machine */
// #define local_meta (dsm_meta->local_meta[CUR_MACHINE_ID]);

static inline void dsm_init_meta(vaddr_t shm_vaddr)
{
    dsm_meta = (dsm_metadata_t *)shm_vaddr;
}

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
