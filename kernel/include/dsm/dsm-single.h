#pragma once

#include <sched/sched.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <common/util.h>
#include <mm/mm.h>
#include <mm/slab.h>
#include <machine.h>

// #define DSM_DEBUG

#define DSM_PREFIX "[DSM]"

#define dsm_info(fmt, ...)  printk(DSM_PREFIX " " fmt, ##__VA_ARGS__)
#define dsm_error(fmt, ...) printk(DSM_PREFIX " " fmt, ##__VA_ARGS__)
#ifdef DSM_DEBUG
#define dsm_debug(fmt, ...) printk(DSM_PREFIX " " fmt, ##__VA_ARGS__)
#else
#define dsm_debug(fmt, ...)
#endif

#define pingpong_info(thread, fmt, ...)                                      \
    if (!strcmp(thread->cap_group->cap_group_name, "/pingpong-pthread.bin")) \
    printk("[pingpong] " fmt, ##__VA_ARGS__)

/**
 * DSM_STATE
 */
enum {
    DSM_CONFIG_STATE_UNINITED = 0,
    DSM_CONFIG_STATE_MM_INITED,
    DSM_CONFIG_STATE_SCHED_INITED,
} dsm_config_state_type;

#define DSM_STATE (dsm_meta->state)

/**
 * cluster machine num
 * when register every machine, it will increase (LOCAL CPU NUM)
 */
#define CLUSTER_MAX_CPU_NUM (1024)
#define CLUSTER_CPU_NUM     (dsm_meta->cluster_cpu_num)

#define CLUSTER_MAX_MACHINE_NUM (16)
#define CLUSTER_MACHINE_NUM     (dsm_meta->cluster_machine_num)

/**
 * machine ID of current machine
 */
u32 machine_id;
#define MACHINE_ID (machine_id)

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

typedef struct {
    // global configuration
    u32 cluster_cpu_num;
    u32 cluster_machine_num;
    volatile u64 state;

    /**
     * dsm memory layout:
     * kernel space:
     * shm_vaddr                                 max_vaddr
     *     | SHM | M1 LOCAL MEM | ... | Mn LOCAL MEM |
     */
    u64 shm_paddr;
    u64 shm_size;
    u64 local_paddr;
    u64 max_paddr; // vaddr of max local DRAM

    /* local mem kernel addr of each machine */
    dsm_machine_local_metadata_t local_meta[CLUSTER_MAX_MACHINE_NUM];

    // after configuration, should be consistent among all machines
    // buddy system
    struct phys_mem_pool mem_pool[N_PHYS_MEM_POOLS];
    // slab system
    struct slab_pointer slab_pool[SLAB_MAX_ORDER + 1];
    struct lock slabs_locks[SLAB_MAX_ORDER + 1];

    // FIXME(FN): remove this ugly tetsing share
    struct shared_queue_meta shared_queue[CLUSTER_MAX_MACHINE_NUM];

#if defined CHCORE_SSI_SLS
    /* crash_last_time = 1 means unexpected */
    bool crash_last_time;
    /* Checkpoint time stamp */
    u64 version_number;
    /* Is doing ckpt (or else is restore) */
    bool ckpt_initialized;
    /* Checkpoint data */
    struct ckpt_ws_table *ckpt_whole_sys_table;
    struct kvs *ckpt_global_obj_map;
    struct slab_pointer slabs[SLAB_MAX_ORDER + 1];
#endif
    struct shared_queue_meta ready_to_merge_object_queue;

    // TODO(yjs): struct
    struct thread *tmpfs_thread[CLUSTER_MAX_MACHINE_NUM];
} __attribute__((aligned(SIZE_1M))) dsm_metadata_t;

dsm_metadata_t *dsm_meta;

/* local meta of current machine */
// #define local_meta (dsm_meta->local_meta[MACHINE_ID]);

static inline void dsm_init_meta(vaddr_t shm_vaddr)
{
    dsm_meta = (dsm_metadata_t *)shm_vaddr;
    init_list_head(&(dsm_meta->ready_to_merge_object_queue.queue_head));
    lock_init(&(dsm_meta->ready_to_merge_object_queue.queue_lock));
    dsm_meta->ready_to_merge_object_queue.queue_len = 0;
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
