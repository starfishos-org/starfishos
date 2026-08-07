# DSM_SHM_DEVICE:
# "IVSHMEM_NUMA" or "CXL_NUMA" or "CXL"
set(DSM_SHM_DEVICE "IVSHMEM")
# DSM_MALLOC_MODE 
# "CXL": all on CXL
# "DRAM": all on DRAM
# "TEMP": all on temp allocator (for debugging / bring-up)
# "MIXED_DEFAULT_DRAM": mixed and default to DRAM
# "MIXED_DEFAULT_CXL": mixed and default to CXL
#
# NOTE: these two are the placement that every experiment which does not set
# its own inherits -- artifact-evaluation/{0-basic,1-ipc-cdf,
# 2-sched-notify-latency,3-memory-allocator,7-recover-fs,9-queue-saturation}
# all build against whatever is committed here, and nothing in those scripts
# would report the difference.  Changing these values silently re-measures
# paper Figure 11a/11b and the allocator figure.  Override per experiment with
# ae_set_dsm_var (see 4-state-partition/run.sh) instead of editing this file.
set(DSM_MALLOC_MODE "MIXED_DEFAULT_CXL")
# "DEFAULT_DRAM": default to DRAM
# "DEFAULT_CXL": default to CXL
set(DSM_USER_MALLOC_MODE "DEFAULT_CXL")

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
set(DSM_CXL_LF_BUDDY "OFF")

# CXL residency hard cap. A fault that would exceed it publishes demand for
# the polling service's asynchronous singleton demoter and waits for headroom.
# The demoter executes at most one bounded batch per kernel entry; there is no
# high/low watermark policy in this initial asynchronous implementation.
set(DSM_CXL_DEMOTE "OFF")
set(DSM_CXL_DEMOTE_LIMIT_MB "4096")

# If "ON", enable per-slab in-flight undo log for crash recovery.
# Adds FLUSH/FENCE overhead on slab alloc/free hot path.
set(SLAB_CRASH_RECOVERY "OFF")

# If "ON", enable cross-machine scheduler timing probes (set_affinity → dequeue latency).
# Requires PHOENIX_SCHED_TIMING in kernel; pairs with PHOENIX_TIMING in user/demos/phoenix-2.0.
set(PHOENIX_SCHED_TIMING "OFF")
