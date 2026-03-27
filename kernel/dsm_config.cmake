# DSM_SHM_DEVICE:
# "IVSHMEM_NUMA" or "CXL_NUMA" or "CXL"
set(DSM_SHM_DEVICE "IVSHMEM")
# DSM_MALLOC_MODE 
# "CXL": all on CXL
# "DRAM": all on DRAM
# "TEMP": all on temp allocator (for debugging / bring-up)
# "MIXED_DEFAULT_DRAM": mixed and default to DRAM
# "MIXED_DEFAULT_CXL": mixed and default to CXL
set(DSM_MALLOC_MODE "MIXED_DEFAULT_CXL")
# "DEFAULT_DRAM": default to DRAM
# "DEFAULT_CXL": default to CXL
set(DSM_USER_MALLOC_MODE "DEFAULT_DRAM")

set(DSM_THREADCTX_MODE "CXL")
set(DSM_PGTABLE_MODE "CXL")
set(DSM_STACK_MODE "CXL")
# Now: always use DRAM for object
set(DSM_OBJECT_MODE "CXL")
set(DSM_PAGE_MODE "CXL")

# If "ON", use ivshmem-plain devices (magic "numaX.Y") as each machine's DRAM.
# If "OFF", use QEMU RAM (-m) and slice it in kernel: [tmp_size, tmp_size+dram_size/DSM_FIXED_MACHINE_NUM).
set(USE_DEV_AS_DRAM "ON")

# If "ON", CXL memory pool uses lock-free buddy allocator; 
# if "OFF", use original lock-based buddy allocator.
set(DSM_CXL_LF_BUDDY "ON")
