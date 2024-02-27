#include "mm/slab.h"
#include <dsm/dsm-single.h>

void dsm_add_machine()
{
        BUG_ON(!dsm_meta);

        /* machine id */
        MACHINE_ID = 
                atomic_fetch_add_32(&(dsm_meta->cluster_machine_num), 1);
        if (dsm_meta->cluster_machine_num > CLUSTER_MAX_MACHINE_NUM)
                BUG("[DSM] machine number exceed max allowed num\n");

        /* cpu range */
        CPU_RANGE_LOW =
                atomic_fetch_add_32(&(dsm_meta->cluster_cpu_num), PLAT_CPU_NUM);
        CPU_RANGE_HIGH = CPU_RANGE_LOW + PLAT_CPU_NUM - 1;

        if (dsm_meta->cluster_cpu_num > CLUSTER_MAX_CPU_NUM)
                BUG("[DSM] cpu number exceed max allowed num\n");

        dsm_meta->local_meta[MACHINE_ID].cpu_range_low = CPU_RANGE_LOW;
        dsm_meta->local_meta[MACHINE_ID].cpu_range_high = CPU_RANGE_HIGH;

#ifdef DSM_LINEAR_MM_LAYOUT
        /* dram */
        u64 lmem_old_start, lmem_new_start, lmem_size;
        lmem_old_start = physmem_map[0][0];
        lmem_size = physmem_map[0][1] - physmem_map[0][0];
        lmem_new_start =
                atomic_fetch_add_64(&(dsm_meta->max_paddr), lmem_size);
        remap_memory(lmem_old_start, lmem_new_start, lmem_size);
        extern void init_buddy_for_one_mem_pool(struct phys_mem_pool * pool,
                                                page_type_t type,
                                                paddr_t free_mem_start,
                                                paddr_t free_mem_end);
        init_buddy_for_one_mem_pool(global_dram_mem[0],
                                    DRAM_PAGE,
                                    lmem_new_start,
                                    lmem_new_start + lmem_size);

        dsm_meta->local_meta[MACHINE_ID].local_mem_start = lmem_new_start;
        dsm_meta->local_meta[MACHINE_ID].local_mem_size = lmem_size;

        kinfo("[DSM] machine %d local memory range: %llx-%llx\n",
              MACHINE_ID,
              lmem_new_start,
              lmem_new_start + lmem_size);
#endif
        kinfo("[DSM] machine %d (cpu%d - cpu%d) join the cluster!\n",
              MACHINE_ID,
              CPU_RANGE_LOW,
              CPU_RANGE_HIGH);
}
