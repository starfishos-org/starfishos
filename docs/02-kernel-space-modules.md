# 2. Kernel-space modules

The kernel extends ChCore capability mechanisms across machines while sharing only the state necessary to hand work to another machine.

## IPC

The capability-IPC interface remains unchanged. For a remote endpoint, the sender transfers execution to a shared/shadow representation, enqueues it for the target, rings the doorbell, and the target resumes server handling. The return path is symmetric.

- [kernel/ipc/connection.c](../kernel/ipc/connection.c): server registration, connections, request/return, capability transfer, and client/server migration.
- [kernel/dsm/dsm_objects/connection.c](../kernel/dsm/dsm_objects/connection.c) and `thread.c`: remote IPC and shadow-thread DSM conversion.
- [kernel/irq/ipi.c](../kernel/irq/ipi.c) and [kernel/drivers/pci/ivshmem.c](../kernel/drivers/pci/ivshmem.c): wake-up transport.

`artifact-evaluation/1-ipc-cdf/` measures empty IPC and filesystem-read IPC.

## Scheduling and notification

The round-robin scheduler preserves local run queues and adds a shared handoff queue. The source copies transferable context to shared state, enqueues it, and notifies the destination; the destination places the thread onto its local CPU queue.

[kernel/sched/policy_rr.c](../kernel/sched/policy_rr.c) contains `rr_sched_migrate_to_remote`, `rr_sched_migrate_from_shared_queue`, `rr_sched_enqueue`, and `rr_sched`. Thread/context support is in `kernel/object/thread.c` and `kernel/sched/context.c`.

[kernel/ipc/notification.c](../kernel/ipc/notification.c) implements `wait_notific`, `signal_notific`, requeueing, timeout cleanup, and the `sys_wait`/`sys_notify` calls. It relies on the same remote scheduling and interrupt path. `artifact-evaluation/2-sched-notify-latency/` measures both mechanisms.

## Memory management

Allocation distinguishes private DRAM from shared CXL. CXL can use a lock-free buddy allocator with per-CPU caches; slab allocators serve small objects.

- [kernel/mm/mm.c](../kernel/mm/mm.c), `dram_alloc.c`, `cxl/cxl_alloc.c`: pool setup and allocation routing.
- [kernel/mm/buddy-lock-free.c](../kernel/mm/buddy-lock-free.c), `kernel/mm/llfree/`: shared CXL page allocator.
- [kernel/mm/cxl/cxl_slab.c](../kernel/mm/cxl/cxl_slab.c): CXL small-object allocator and optional recovery.
- [kernel/mm/pgfault_handler.c](../kernel/mm/pgfault_handler.c), [kernel/dsm/dsm_tiering.c](../kernel/dsm/dsm_tiering.c): page faults, migration, and tiering.

`DSM_CXL_LF_BUDDY` and `SLAB_CRASH_RECOVERY` select the main allocator variants. `artifact-evaluation/3-memory-allocator/` builds the needed configurations.

The corresponding paper diagram is included as [cxl-memory-allocator.pdf](assets/cxl-memory-allocator.pdf); it is useful when following the buddy/slab split in the source tree.

## Partial-failure handling

The recovery target is detectable, repairable shared coordination state—not a rollback of every process. Queue users must discard/recover stalled work, allocator metadata has recovery paths, and local schedulers on surviving machines continue. Relevant paths include `kernel/mm/cxl/cxl_slab.c`, `kernel/mm/buddy.c`, `kernel/mm/buddy-lock-free.c`, and `kernel/dsm/`.
