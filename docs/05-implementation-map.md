# 5. Implementation map

| Paper mechanism | Source entry points | Notes |
| --- | --- | --- |
| Pod boot/layout | `kernel/arch/x86_64/main.c`, `build/simulate.sh`, `dsm-scripts/config_memdev.sh` | boot arguments and backing-file topology |
| Shared memory/doorbell | `kernel/drivers/pci/ivshmem.c`, `kernel/irq/ipi.c` | ivshmem state and remote wakeup |
| DSM objects | `kernel/dsm/dsm_metadata.c`, `kernel/dsm/dsm_objects/` | placement and cross-machine conversion |
| Cross-machine IPC | `kernel/ipc/connection.c`, `kernel/dsm/dsm_objects/{connection,thread}.c` | shadow-thread IPC |
| Scheduling | `kernel/sched/policy_rr.c` | shared handoff then local run queue |
| Notification | `kernel/ipc/notification.c` | wait, signal, requeue, timeout |
| CXL allocation | `kernel/mm/buddy-lock-free.c`, `kernel/mm/llfree/`, `kernel/mm/cxl/` | lock-free pages and slabs |
| Tiering/migration | `kernel/dsm/dsm_tiering.c`, `kernel/mm/pgfault_handler.c`, `kernel/dsm/dsm_migrate.c` | data movement and races |
| Service startup | `user/system-servers/procmgr/` | primary/secondary service bootstrap |
| Filesystem/recovery | `user/system-servers/{fs_base,tmpfs}/` | POSIX IPC, p-log, CXL restoration |
| Experiments | `artifact-evaluation/` | executable evaluation scripts |

## Configuration ownership

1. `.config`: platform, SSI, services, and demos.
2. `kernel/dsm_config.cmake`: placement and allocator policy.
3. `build/simulate.sh`: run-time pod sizing/QEMU arguments.
4. `artifact-evaluation/*/run.sh`: experiment-specific instrumentation/build variants.

A run-time environment variable does not change a compile-time policy; allocator experiments rebuild explicitly.

