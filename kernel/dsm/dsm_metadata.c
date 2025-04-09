#include "mm/mm.h"
#include <dsm/dsm-single.h>

void dsm_add_machine()
{
    BUG_ON(!dsm_meta);

    /* machine id */
    CUR_MACHINE_ID = atomic_fetch_add_32(&(dsm_meta->cluster_machine_num), 1);
    if (dsm_meta->cluster_machine_num > CLUSTER_MAX_MACHINE_NUM)
        BUG("[DSM] machine number exceed max allowed num\n");

    /* cpu range */
    CPU_RANGE_LOW =
            atomic_fetch_add_32(&(dsm_meta->cluster_cpu_num), PLAT_CPU_NUM);
    CPU_RANGE_HIGH = CPU_RANGE_LOW + PLAT_CPU_NUM - 1;

    if (dsm_meta->cluster_cpu_num > CLUSTER_MAX_CPU_NUM)
        BUG("[DSM] cpu number exceed max allowed num\n");

    dsm_meta->local_meta[CUR_MACHINE_ID].cpu_range_low = CPU_RANGE_LOW;
    dsm_meta->local_meta[CUR_MACHINE_ID].cpu_range_high = CPU_RANGE_HIGH;

#ifdef DSM_LINEAR_MM_LAYOUT
    /* dram */
    // u64 lmem_old_start,
    u64 lmem_new_start, lmem_size = SIZE_8G;

    lmem_new_start = atomic_fetch_add_64(&(dsm_meta->max_paddr), lmem_size);
    kinfo("[DSM] machine %d local memory range: %llx-%llx\n",
          CUR_MACHINE_ID,
          lmem_new_start,
          lmem_new_start + lmem_size);

    /* local memory range exceed shm range */
    BUG_ON(dsm_meta->max_paddr > dsm_meta->shm_paddr);

    physmem_map[0][0] = lmem_new_start;
    physmem_map[0][1] = lmem_new_start + lmem_size;

    // remap_memory(lmem_old_start, lmem_new_start, lmem_size);

    extern void flush_tlb_all();
    flush_tlb_all();

    extern void dram_mm_init();
    dram_mm_init();

    dsm_meta->local_meta[CUR_MACHINE_ID].local_mem_start = lmem_new_start;
    dsm_meta->local_meta[CUR_MACHINE_ID].local_mem_size = lmem_size;

    // kinfo("[DSM] machine %d local memory range: %llx-%llx (old:
    // %llx-%llx)\n",
    //       CUR_MACHINE_ID,
    //       lmem_new_start,
    //       lmem_new_start + lmem_size,
    //       lmem_old_start,
    //       lmem_old_start + lmem_size);
    kinfo("[DSM] machine %d local memory range: %llx-%llx\n",
          CUR_MACHINE_ID,
          lmem_new_start,
          lmem_new_start + lmem_size);
#endif
    kinfo("\033[31m"
          "\r[DSM] machine %d (cpu%d - cpu%d) join the cluster!\n"
          "\033[0m",
          CUR_MACHINE_ID,
          CPU_RANGE_LOW,
          CPU_RANGE_HIGH);
}
