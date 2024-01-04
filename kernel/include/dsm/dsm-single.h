#pragma once

#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <common/util.h>
#include <mm/mm.h>
#include <mm/slab.h>
#include <machine.h>

/**
 * cluster machine nume
 * when register every machine, it will increase (LOCAL CPU NUM)
 */
#define CLUSTER_MAX_CPU_NUM (128)
#define CLUSTER_CPU_NUM     (dsm_meta->cluster_cpu_num)

/**
 * cpu range of current machine
 */
u32 cpu_range_low, cpu_range_high;

#define CPU_RANGE_LOW  (cpu_range_low)
#define CPU_RANGE_HIGH (cpu_range_high)

struct shared_queue_meta {
        struct list_head queue_head;
        u32 queue_len;
        struct lock queue_lock;
};

typedef struct {
        // global configuration
        u64 cluster_cpu_num;
        enum {
                DSM_CONFIG_STATE_UNINITED = 0,
                DSM_CONFIG_STATE_INITED,
        } dsm_config_state_type;
        volatile u64 state;
        // after configuration, should be consistent among all machines
        // buddy system
        struct phys_mem_pool mem_pool[N_PHYS_MEM_POOLS];
        // slab system
        struct slab_pointer slab_pool[SLAB_MAX_ORDER + 1];
        struct lock slabs_locks[SLAB_MAX_ORDER + 1];

        // FIXME(FN): remove this ugly tetsing share
        struct shared_queue_meta shared_queue[CLUSTER_MAX_CPU_NUM];
} __attribute__((aligned(SIZE_1M))) dsm_metadata_t;

dsm_metadata_t *dsm_meta;

static inline void dsm_init_meta(vaddr_t start_addr)
{
        dsm_meta = (dsm_metadata_t *)start_addr;
}

static inline u64 dsm_is_inited()
{
        BUG_ON(!dsm_meta);
        return (dsm_meta->state == DSM_CONFIG_STATE_INITED);
}

static inline void dsm_mark_inited()
{
        BUG_ON(!dsm_meta);
        dsm_meta->state = DSM_CONFIG_STATE_INITED;
}

static inline void dsm_add_machine()
{
        BUG_ON(!dsm_meta);
        CPU_RANGE_LOW =
                atomic_fetch_add_32(&(dsm_meta->cluster_cpu_num), PLAT_CPU_NUM);
        CPU_RANGE_HIGH = CPU_RANGE_LOW + PLAT_CPU_NUM - 1;

        if (dsm_meta->cluster_cpu_num > CLUSTER_MAX_CPU_NUM)
                BUG("[DSM] cpu number exceed max allowed num\n");

        kinfo("[DSM] machine (cpu%d - cpu%d) join the cluster!\n",
              CPU_RANGE_LOW,
              CPU_RANGE_HIGH);
}
