#include <dsm/dsm-single.h>
#include <common/macro.h>
#include <common/size.h>
#include <lib/fw_cfg.h>
#ifdef PHOENIX_SCHED_TIMING
#include <arch/time.h>
#endif

int machine_id = -1;

void dsm_add_machine()
{
    BUG_ON(!dsm_meta);

    /* Initialize machine_to_peer_id array if this is the first machine */
    if (CUR_MACHINE_ID == 0) {
        for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
            dsm_meta->machine_to_peer_id[i] = 0xFFFFFFFF; /* Uninitialized */
        }
    }

    if (CUR_MACHINE_ID > dsm_meta->cluster_machine_num) {
        BUG("[DSM] machine id exceed\n");
    }

    /* Guard all dsm_meta->local_meta[CUR_MACHINE_ID] accesses below. */
    if (CUR_MACHINE_ID >= CLUSTER_MAX_MACHINE_NUM) {
        BUG("[DSM] machine id exceed max allowed num\n");
    }

    int init = CUR_MACHINE_ID == dsm_meta->cluster_machine_num;

    if (init) {
        atomic_fetch_add_32(&(dsm_meta->cluster_machine_num), 1);
        if (dsm_meta->cluster_machine_num > CLUSTER_MAX_MACHINE_NUM) {
            BUG("[DSM] machine number exceed max allowed num\n");
        }
        CPU_RANGE_LOW =
                atomic_fetch_add_32(&(dsm_meta->cluster_cpu_num), PLAT_CPU_NUM);
        CPU_RANGE_HIGH = CPU_RANGE_LOW + PLAT_CPU_NUM - 1;
        if (dsm_meta->cluster_cpu_num > CLUSTER_MAX_CPU_NUM) {
            BUG("[DSM] cpu number exceed max allowed num\n");
        }
    } else {
        CPU_RANGE_LOW = CUR_MACHINE_ID * PLAT_CPU_NUM;
        CPU_RANGE_HIGH = CPU_RANGE_LOW + PLAT_CPU_NUM - 1;
    }

    dsm_meta->local_meta[CUR_MACHINE_ID].cpu_range_low = CPU_RANGE_LOW;
    dsm_meta->local_meta[CUR_MACHINE_ID].cpu_range_high = CPU_RANGE_HIGH;

    u64 lmem_new_start, lmem_size;

    lmem_new_start = 0;
    lmem_size = 0;

#ifdef USE_DEV_AS_DRAM
    extern u64 dram_devices_map[][2];
    lmem_new_start = dram_devices_map[CUR_MACHINE_ID][0];
    lmem_size = dram_devices_map[CUR_MACHINE_ID][1];
#else
    extern paddr_t temp_mem_start;
    extern u64 temp_mem_size;

    u64 dram_size = FW_DRAM_SIZE_BYTES;
    int machine_num = FW_MACHINE_NUM;

    lmem_new_start = (u64)temp_mem_start + (u64)temp_mem_size;
    if (dram_size == 0)
        BUG("[DSM] dram_size not provided in bootargs\n");

    if (machine_num <= 0)
        machine_num = DSM_FIXED_MACHINE_NUM;

    lmem_size = dram_size / (u64)machine_num;
    if (lmem_size == 0)
        BUG("[DSM] dram_size too small for machine_num=%d\n", machine_num);
#endif

    kinfo("[DSM] machine %d local memory range: %llx-%llx (size: %llx)\n",
          CUR_MACHINE_ID,
          lmem_new_start,
          lmem_new_start + lmem_size,
          lmem_size);

    extern void flush_tlb_all();
    flush_tlb_all();

    physmem_map[0][0] = lmem_new_start;
    physmem_map[0][1] = lmem_new_start + lmem_size;
    physmem_map_num = 1;
    kinfo(ANSI_COLOR_MAGENTA "[DRAM MEMORY] machine %d local memory range: %llx-%llx" ANSI_COLOR_RESET "\n",
          CUR_MACHINE_ID,
          lmem_new_start,
          lmem_new_start + lmem_size);
    extern void dram_mm_init();
    dram_mm_init();

    extern void fill_kernel_page_table_range(u64 mem_start, u64 mem_size);
    fill_kernel_page_table_range(lmem_new_start, lmem_size);

    dsm_meta->local_meta[CUR_MACHINE_ID].local_mem_start = lmem_new_start;
    dsm_meta->local_meta[CUR_MACHINE_ID].local_mem_size = lmem_size;

    kdebug("[DSM] machine %d local memory range: %llx-%llx\n",
          CUR_MACHINE_ID,
          lmem_new_start,
          lmem_new_start + lmem_size);

    kinfo(ANSI_COLOR_GREEN ANSI_COLOR_BOLD
          "\r[DSM] machine %d (cpu%d - cpu%d) join the cluster!\n"
          ANSI_COLOR_RESET,
          CUR_MACHINE_ID, CPU_RANGE_LOW, CPU_RANGE_HIGH);

#ifdef PHOENIX_SCHED_TIMING
    /*
     * Cross-machine TSC calibration barrier.
     * All machines signal ready, spin until everyone is ready, then
     * write get_cycles() simultaneously.  After this barrier,
     * dsm_tsc_to_m0() can convert any machine's raw TSC to machine-0's
     * time domain with < 1 µs error.
     */
    {
        int _n = FW_MACHINE_NUM > 0 ? FW_MACHINE_NUM : 1;
        dsm_meta->tsc_sync_ready[CUR_MACHINE_ID] = 1;
        /* Wait for all machines to mark ready */
        for (int _i = 0; _i < _n; _i++)
            while (!dsm_meta->tsc_sync_ready[_i]) {}
        /* All machines write their TSC at roughly the same real time */
        dsm_meta->tsc_sync[CUR_MACHINE_ID] = get_cycles();
        kinfo("[SCHED_TIMING] machine %d tsc_sync=%llu\n",
              CUR_MACHINE_ID, dsm_meta->tsc_sync[CUR_MACHINE_ID]);
    }
#endif
}
