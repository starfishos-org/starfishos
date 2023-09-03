# Implementation of TreeSLS

We implemented TreeSLS based on ChCore, which is an educational multicore microkernel that supports POSIX APIs through musl-libc.

TreeSLS's modification includes: 
- checkpoint/restore module (`kernel/ckpt`)
- memory allocator module (`kernel/mm`)
- ipi module (`kernel/ipi`)
- several new syscalls (`kernel/syscall`)
- user applications (listed in `user/sample-apps/apps/treesls`)

## Checkpoint/Restore

TreeSLS adds a checkpoint/restore module (`kernel/ckpt`).
Whole-system checkpoint/restore can be taken through `sys_whole_ckpt` (`kernel/ckpt/ckpt.c`) and `sys_whole_restore` (`kernel/ckpt/restore.c`).

Hybrid page checkpointing is done through `process_sub_active_list()`, which is called in parallel to the main process of checkpointing.

## Memory Allocator

TreeSLS modifies the memory allocator (`kernel/mm`) by adding a lightweight journal when malloc and free pages from the buddy system.
The journal is taken through `prepare_latest_log()` in every malloc/free function.

## Other Modules (IPI, syscall, etc.)

TreeSLS also modified some other modules (IPI, syscall, etc.) to enable checkpointing.

1. IPI: add `sys_ipi_stop_all` and `sys_ipi_start_all`.
2. syscall: export `sys_whole_ckpt`, `sys_whole_restore`, and several supporting syscalls.
