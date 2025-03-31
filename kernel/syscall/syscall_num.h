#pragma once

#define NR_SYSCALL 512

/* Character */
#define SYS_putc 0
#define SYS_getc 1

/* PMO */
/* - single */
#define SYS_create_pmo        10
#define SYS_create_device_pmo 11
#define SYS_map_pmo           12
#define SYS_unmap_pmo         13
#define SYS_write_pmo         14
#define SYS_read_pmo          15
#define SYS_map_with_pmo      16
#define SYS_unmap_with_addr   17
#define SYS_revoke_cap        18
/* - batch */
#define SYS_create_pmos 20
#define SYS_map_pmos    21
/* - address translation */
#define SYS_get_pmo_paddr 30
#define SYS_get_phys_addr 31

/* Capability */
#define SYS_cap_copy_to   60
#define SYS_cap_copy_from 61
#define SYS_transfer_caps 62
/* Fork */
#define SYS_clone_cap_group 70

/* Multitask */
/* - create & exit */
#define SYS_create_cap_group 80
#define SYS_exit_group       81
#define SYS_create_thread    82
#define SYS_thread_exit      83
/* - recycle */
#define SYS_register_recycle  90
#define SYS_cap_group_recycle 91
/* - schedule */
#define SYS_yield        100
#define SYS_set_affinity 101
#define SYS_get_affinity 102

/* IPC */
/* - procedure call */
#define SYS_register_server        120
#define SYS_register_client        121
#define SYS_ipc_register_cb_return 122
#define SYS_ipc_call               123
#define SYS_ipc_return             124
#define SYS_ipc_send_cap           125
/* - notification */
#define SYS_create_notifc 130
#define SYS_wait          131
#define SYS_notify        132

/* Exception */
/* - irq */
#define SYS_irq_register 150
#define SYS_irq_wait     151
#define SYS_irq_ack      152
/* - page fault */
#define SYS_user_fault_register 165
#define SYS_user_fault_map      166

/* Hardware Access (Privileged Instruction) */
/* - cache */
#define SYS_cache_flush 180
/* - timer */
#define SYS_get_current_tick 185

/* POSIX */
/* - time */
#define SYS_clock_gettime   200
#define SYS_clock_nanosleep 201
/* - memory */
#define SYS_handle_brk      210
#define SYS_handle_mprotect 213

/* Debug */
#define SYS_debug_va          220
#define SYS_top               221
#define SYS_get_free_mem_size 222

/* Performance Benchmark */
#define SYS_perf_start 230
#define SYS_perf_end   231
#define SYS_perf_null  232

#ifdef CHCORE_SLS
#define SYS_get_poll_remote                   233
#define SYS_set_poll_remote                   234
#define SYS_set_excepted_connected_client_num 235
#define SYS_set_dyn_args 236
#endif

/* PCIe BUS */
#define SYS_pcie_control 237

/* SHUTDOWN */
#define SYS_shutdown 243

/* Virtualization */
#define SYS_virt_dispatch 240

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
/* Checkpoint */
#define SYS_whole_ckpt                241
#define SYS_whole_restore             242
#define SYS_shutdown                  243
#define SYS_whole_ckpt_for_test       244
#define SYS_register_external_ringbuf 245

/* IPI */
#define SYS_ipi_stop_all    246
#define SYS_ipi_start_all   247
#define SYS_ipi_test_kernel 248
#endif

/* track pf */
#define SYS_track_pf_begin 249
#define SYS_track_pf_end   250

#ifdef CHCORE_SSI_SLS
#define SYS_cfork_prepare 251
#define SYS_cfork_ckpt    252
#define SYS_cfork_restore 253
#endif

/* FUTEX */
#define SYS_futex 254
#define SYS_set_tid_address 255
