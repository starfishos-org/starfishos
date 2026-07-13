#pragma once

/* Use -1 instead of 0 (NULL) since 0 is used as the ending mark of envp */
#define ENVP_NONE_PMOS (-1)
#define ENVP_NONE_CAPS (-1)

/* Used for usys_fs_load_cpio */
/* wh_cpio */
#define FSM_CPIO     0
#define TMPFS_CPIO   1
#define RAMDISK_CPIO 2
/* op */
#define QUERY_SIZE 0
#define LOAD_CPIO  1

/*
 * Used as a token which is added at the beginnig of the path name
 * during booting a system server. E.g., "kernel-server/fsm.srv"
 *
 * This is simply for preventing users unintentionally
 * run system servers in Shell. That is meaningless.
 */
#define KERNEL_SERVER "kernel-server"

#define NO_AFF       (-1)
#define NO_ARG       (0)
#define PASSIVE_PRIO (-1)

/* affinity of each system server */
#define SHELL_AFF   (2)
#define PROCMGR_AFF (3)
#define LWIP_AFF    (4)

/*
 * TODO (tmac): uapi can help us to clean the code,
 * i.e., only keepping one MARCO for both kernel and user.
 * TODO(FN): move to uapi.h
 */

/* cache operations */
#define CACHE_CLEAN         1
#define CACHE_INVALIDATE    2
#define CACHE_CLEAN_AND_INV 3
#define SYNC_IDCACHE        4

/* virtual memory rights */
#define VM_READ   (1 << 0)
#define VM_WRITE  (1 << 1)
#define VM_EXEC   (1 << 2)
#define VM_FORBID (0)

/* PMO types */
#define PMO_ANONYM       0 /* lazy allocation */
#define PMO_DATA         1 /* immediate allocation */
#define PMO_FILE         2 /* file backed */
#define PMO_SHM          3 /* shared memory */
#define PMO_USER_PAGER   4 /* support user pager */
#define PMO_DEVICE       5 /* memory mapped device registers */
#define PMO_DATA_NOCACHE 6 /* non-cacheable immediate allocation */
#define PMO_FORBID       7 /* Forbidden area: avoid overflow */

// Following type are actually mapped to previous types
#define PMO_RING_BUFFER 8 /* pages that need to sync with external, PMO_DATA \
                           */
#define PMO_RING_BUFFER_RADIX \
    9 /* same as PMO_RING_BUFFER; for test, PMO_ANONYM */
// More types for partioned process
#define PMO_CODE  10 /* code, PMO_DATA */
#define PMO_STACK 11 /* stack, PMO_ANONYM */
#define PMO_HEAP  12 /* heap, PMO_ANONYM */
// #define PMO_IPC_BUFFER        13 /* ipc buffer, PMO_SHM */
#define PMO_CROSS_SHM 14 /* shared memory accross machine, PMO_SHM */
#define PMO_TYPE_NR   15

#define MALLOC_TYPE_PRIVATE (1)
#define MALLOC_TYPE_SHARED  (2)
#define MALLOC_TYPE_DEFAULT (3)

/* a thread's own cap_group */
#define SELF_CAP 0

#define DEFAULT_PRIO 1

/* TODO: use something like **vmalloc** to remvoe the magic adresses */
#define CHILD_THREAD_STACK_BASE (0x500000800000UL)
#define CHILD_THREAD_STACK_SIZE (0x800000UL)
#define CHILD_THREAD_PRIO       (DEFAULT_PRIO)

#define MAIN_THREAD_STACK_BASE (0x500000000000UL)
#define MAIN_THREAD_STACK_SIZE (0x800000UL)
#define MAIN_THREAD_PRIO       (DEFAULT_PRIO)

#define IPC_PER_SHM_SIZE (0x1000)

#define ROUND_UP(x, n)   (((x) + (n)-1) & ~((n)-1))
#define ROUND_DOWN(x, n) ((x) & ~((n)-1))

#ifndef PAGE_SIZE
#define PAGE_SIZE 0x1000
#endif

/* Syscall numbers */

/* Character */
#define CHCORE_SYS_putc 0
#define CHCORE_SYS_getc 1

/* PMO */
/* - single */
#define CHCORE_SYS_create_pmo        10
#define CHCORE_SYS_create_device_pmo 11
#define CHCORE_SYS_map_pmo           12
#define CHCORE_SYS_unmap_pmo         13
#define CHCORE_SYS_write_pmo         14
#define CHCORE_SYS_read_pmo          15
#define CHCORE_SYS_map_with_pmo      16
#define CHCORE_SYS_unmap_with_addr   17
#define CHCORE_SYS_revoke_cap        18
/* - batch */
#define CHCORE_SYS_create_pmos 20
#define CHCORE_SYS_map_pmos    21
/* - address translation */
#define CHCORE_SYS_get_pmo_paddr 30
#define CHCORE_SYS_get_phys_addr 31

/* Capability */
#define CHCORE_SYS_cap_copy_to   60
#define CHCORE_SYS_cap_copy_from 61
#define CHCORE_SYS_transfer_caps 62
/* Fork */
#define CHCORE_SYS_clone_cap_group 70

/* Multitask */
/* - create & exit */
#define CHCORE_SYS_create_cap_group 80
#define CHCORE_SYS_exit_group       81
#define CHCORE_SYS_create_thread    82
#define CHCORE_SYS_thread_exit      83
/* - recycle */
#define CHCORE_SYS_register_recycle  90
#define CHCORE_SYS_cap_group_recycle 91
/* - schedule */
#define CHCORE_SYS_yield        100
#define CHCORE_SYS_set_affinity 101
#define CHCORE_SYS_get_affinity 102

/* IPC */
/* - procedure call */
#define CHCORE_SYS_register_server        120
#define CHCORE_SYS_register_client        121
#define CHCORE_SYS_ipc_register_cb_return 122
#define CHCORE_SYS_ipc_call               123
#define CHCORE_SYS_ipc_return             124
#define CHCORE_SYS_ipc_send_cap           125
/* - notification */
#define CHCORE_SYS_create_notifc 130
#define CHCORE_SYS_wait          131
#define CHCORE_SYS_notify        132
/* - futex */
#define CHCORE_SYS_futex           133
#define CHCORE_SYS_set_tid_address 134

/* Exception */
/* - irq */
#define CHCORE_SYS_irq_register 150
#define CHCORE_SYS_irq_wait     151
#define CHCORE_SYS_irq_ack      152
/* - page fault */
#define CHCORE_SYS_user_fault_register 165
#define CHCORE_SYS_user_fault_map      166

/* Hardware Access (Privileged Instruction) */
/* - cache */
#define CHCORE_SYS_cache_flush 180
/* - timer */
#define CHCORE_SYS_get_current_tick 185

/* POSIX */
/* - time */
#define CHCORE_SYS_clock_gettime   200
#define CHCORE_SYS_clock_nanosleep 201
/* - memory */
#define CHCORE_SYS_handle_brk      210
#define CHCORE_SYS_handle_mprotect 213

/* CPIO */
/* TODO(qyc): to be deprecated */
#define CHCORE_SYS_fs_load_cpio 215

/* Debug */
#define CHCORE_SYS_debug_va          220
#define CHCORE_SYS_top               221
#define CHCORE_SYS_get_free_mem_size 222

/* Performance Benchmark */
#define CHCORE_SYS_perf_start 230
#define CHCORE_SYS_perf_end   231
#define CHCORE_SYS_perf_null  232

#define CHCORE_SYS_get_poll_remote                   233
#define CHCORE_SYS_set_poll_remote                   234
#define CHCORE_SYS_set_excepted_connected_client_num 235
#define CHCORE_SYS_set_dyn_args                      236

/* PCIe BUS */
#define CHCORE_SYS_pcie_control 237

/* Virtualization */
#define CHCORE_SYS_virt_dispatch 240

/* Checkpoint */
#define CHCORE_SYS_whole_ckpt                241
#define CHCORE_SYS_whole_restore             242
#define CHCORE_SYS_shutdown                  243
#define CHCORE_SYS_whole_ckpt_for_test       244
#define CHCORE_SYS_register_external_ringbuf 245

/* IPI */
#define CHCORE_SYS_ipi_stop_all    246
#define CHCORE_SYS_ipi_start_all   247
#define CHCORE_SYS_ipi_test_kernel 248

/* track pf */
#define CHCORE_SYS_track_pf_begin 249
#define CHCORE_SYS_track_pf_end   250

#define CHCORE_SYS_cfork_prepare 251
#define CHCORE_SYS_cfork_ckpt    252
#define CHCORE_SYS_cfork_restore 253

#define CHCORE_SYS_ckpt_process    254
#define CHCORE_SYS_restore_process 255

/* Machine ID */
#define CHCORE_SYS_get_machine_id 256
#define CHCORE_SYS_get_machine_cpu_count 263

/* File System */
#define CHCORE_SYS_register_fs_client 257
#define CHCORE_SYS_register_fs_server 258

#ifdef IPC_PERF_ENABLED
#define CHCORE_SYS_ipc_perf_start 259
#define CHCORE_SYS_ipc_perf_end   260
#endif

/* Shared Memory */
#define CHCORE_SYS_mmap_shm             261
#define CHCORE_SYS_memcpy_and_flush_tlb 262
#define CHCORE_SYS_memcpy_and_flush_tlb_batch 265

/* Virtual Memory */
#define CHCORE_SYS_print_vmspace_stats  264

/* Scheduling control */
#define CHCORE_SYS_set_thread_budget    266
#define CHCORE_SYS_ivshmem_msi_bench    267
