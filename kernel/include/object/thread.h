#pragma once

#include <common/list.h>
#include <mm/vmspace.h>
#include <sched/sched.h>
#include <object/cap_group.h>
#include <arch/machine/smp.h>
#include <ipc/connection.h>
#include <irq/timer.h>
#include <common/debug.h>

extern struct thread *current_threads[PLAT_CPU_NUM];
#define current_thread (current_threads[smp_get_cpu_id()])

extern bool resched_flags[PLAT_CPU_NUM];
#define current_resched_flag (resched_flags[smp_get_cpu_id()])

#define current_thread_name (current_thread->cap_group->cap_group_name)

#ifdef CHCORE_KERNEL_RT
/* RT (kernel PREEMT): allocate the stack for each thread  */
#define DEFAULT_KERNEL_STACK_SZ (0x1000)
#else
/* No kernel PREEMT: no stack for each thread, instead, using a per-cpu stack */
#define DEFAULT_KERNEL_STACK_SZ (0)
#endif

#define THREAD_ITSELF ((void *)(-1))

struct thread {
    struct list_head node; // link threads in a same cap_group
    struct list_head ready_queue_node; // link threads in a ready queue
#ifdef DSM_ENABLED
    struct list_head shared_queue_node; // link threads in shared queue node for
                                        // migration
#endif
    struct list_head notification_queue_node; // link threads in a notification
                                              // waiting queue
    struct thread_ctx *thread_ctx; // thread control block

    /*
     * prev_thread switch CPU to this_thread
     *
     * When previous thread is the thread itself,
     * prev_thread will be set to THREAD_ITSELF.
     */
    struct thread *prev_thread;

    /*
     * vmspace: virtual memory address space.
     * vmspace is also stored in the 2nd slot of capability
     */
    struct vmspace *vmspace;

    struct cap_group *cap_group;

    /* Record the thread cap for quick thread recycle. */
    u64 cap;

    /**
     * Only exists for threads in a server process.
     * If not NULL, it points to one of the three config types.
     * 1. @struct ipc_server_config server_config
     *    If the thread declares an IPC service by invoking "register_server"
     * 2. @struct ipc_server_register_cb_config server_register_cb_config
     *    If the thread is TYPE_SHADOW and is used as ipc_server_register_cb_thread
     * 3. @struct ipc_server_handler_config server_handler_config
     *    If the thread is TYPE_SHADOW and is used as ipc_server_handler_thread
     */
    void *general_ipc_config;

#if TRACK_THREAD_MM == ON
    u64 mm_size;
#endif

#ifdef TRACK_TIME
    int tracking; // 0 neither, 1 kernel, 2 user
    u64 timepoint_ns;
    u64 track_time_user;
    u64 track_time_kernel;
#endif

    struct sleep_state sleep_state;

    /* Machine ID of the thread */
    mid_t machine_id;
};

void create_root_thread(void);
void switch_thread_vmspace_to(struct thread *);
void thread_deinit(void *thread_ptr);

/* Syscalls */
int sys_create_thread(u64 thread_args_p);
void sys_thread_exit(void);
int sys_set_affinity(u64 thread_cap, s32 aff);
s32 sys_get_affinity(u64 thread_cap);

/* Fork */
void thread_clone(struct cap_group *cap_group, struct thread *thread);

#ifndef DSM_ENABLED
#undef cpuid_l2g
#define cpuid_l2g(x) (x)
#undef cpuid_g2l
#define cpuid_g2l(x) (x)
#endif
