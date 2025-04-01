/* Scheduler related functions are implemented here */
#include "lib/printk.h"
#include <sched/sched.h>
#include <arch/machine/smp.h>
#include <arch/ipi.h>
#include <common/kprint.h>
#include <machine.h>
#include <mm/kmalloc.h>
#include <common/list.h>
#include <common/util.h>
#include <object/thread.h>
#include <common/macro.h>
#include <common/errno.h>
#include <common/lock.h>
#include <common/bitops.h>
#include <object/thread.h>
#include <irq/irq.h>
#include <irq/ipi.h>
#include <sched/context.h>
#include <sched/fpu.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#endif

struct thread *current_threads[PLAT_CPU_NUM];
struct thread idle_threads[PLAT_CPU_NUM];
bool resched_flags[PLAT_CPU_NUM];

// #if PLAT_CPU_NUM <= 32
u32 resched_bitmaps[PLAT_CPU_NUM];
// #elif
// #error TODO: support more CPUs
// #endif

/* For TLB maintenence */
extern void record_history_cpu(struct vmspace *vmspcae, u32 cpuid);

/* Chosen Scheduling Policies */
struct sched_ops *cur_sched_ops;

char thread_type[][TYPE_STR_LEN] =
        {"IDLE", "KERNEL", "USER", "SHADOW", "REGISTER", "TESTS", "SERVICES"};

char thread_state[][STATE_STR_LEN] = {
        "TS_INIT      ",
        "TS_READY     ",
        "TS_INTER     ",
        "TS_RUNNING   ",
        "TS_EXIT      ",
        "TS_WAITING   ",
        "TS_WAITING_IPC",
};

void print_thread(struct thread *thread)
{
    printk("Thread %p\tType: %s\tState: %s\tCPU %d\tAFF %d\t"
           "Budget %d\tPrio: %d\tIP: %p\tCMD: %s\n",
           thread,
           thread_type[thread->thread_ctx->type],
           thread_state[thread->thread_ctx->state],
           thread->thread_ctx->cpuid,
           thread->thread_ctx->affinity,
           /* REGISTER and SHADOW threads may have no sc, so just print -1.
            */
           thread->thread_ctx->sc ? thread->thread_ctx->sc->budget : -1,
           thread->thread_ctx->prio,
           arch_get_thread_next_ip(thread),
           thread->cap_group->cap_group_name);

#if TRACK_THREAD_MM == ON
    printk("Thread Mem_size:0x%lx\n", thread->mm_size);
#endif
}

void thread_measure(struct thread *thread, int tracking_state);

/*
 * Switch Thread to the specified one.
 * Set the correct thread state to running and
 * the per_cpu varible `current_thread`.
 *
 * Note: the switch is between current_thread and target.
 */
int switch_to_thread(struct thread *target)
{
    BUG_ON(!target);
    BUG_ON(!target->thread_ctx);
    BUG_ON((target->thread_ctx->state == TS_READY));
    BUG_ON((target->thread_ctx->thread_exit_state == TE_EXITED));

    u32 cpuid = smp_get_cpu_id();

#ifdef CHCORE_KERNEL_RT
    resched_bitmaps[cpuid] &= ~BIT(cpuid);
#endif

    /* No thread switch happens actually */
    if (target == current_thread) {
        target->thread_ctx->state = TS_RUNNING;

        /* The previous thread is the thread itself */
        target->prev_thread = THREAD_ITSELF;
        return 0;
    }

    target->thread_ctx->cpuid = cpuid;

#if 0
        // TODO: send IPI to save FPU if migrating a thread from other CPUs
        int is_fpu_owner = target->thread_ctx->is_fpu_owner;
        if ((is_fpu_owner >= 0)
            && (is_fpu_owner != target->thread_ctx->cpuid)) {
                dsm_info("target %p is_fpu_owner %d, local cpu id %d, affinity %d\n",
                      target,
                      is_fpu_owner,
                      target->thread_ctx->cpuid,
                      target->thread_ctx->affinity);
                BUG("WRONG FPU in sched\n");
        }
#endif

    target->thread_ctx->state = TS_RUNNING;

    /* Record the thread transferring the CPU */
    target->prev_thread = current_thread;

#if FPU_SAVING_MODE == EAGER_FPU_MODE
    save_fpu_state(current_thread);
    restore_fpu_state(target);
#else
    /* FPU_SAVING_MODE == LAZY_FPU_MODE */
    if (target->thread_ctx->type > TYPE_KERNEL)
        disable_fpu_usage();
#endif

    /*
     * Switch the TLS information:
     * Save the tls info for current_thread,
     * and restore the tls info for target.
     */
    switch_tls_info(current_thread, target);

    target->thread_ctx->kernel_stack_state = KS_LOCKED;

#ifdef TRACK_TIME
    if (current_thread && current_thread->thread_ctx->type != TYPE_IDLE) {
        thread_measure(current_thread, 0);
    }
#endif

    /*
     * An important assumption: Per-CPU variable current_thread
     * is only accessed by its owner CPU.
     *
     * TODO: Otherwise, consider about using barrier.
     */
    // smp_wmb();
    current_thread = target;

    return 0;
}

/*
 * An externeal interface for used in other places of the kernel,
 * e.g., IPC, notification.
 * Note that this function never return back.
 */
void sched_to_thread(struct thread *target)
{
#ifdef DSM_ENABLED
    /* sched to a thread already migrate to remote */
    if (target->thread_ctx->thread_exit_state == TE_MIGRATING) {
        dsm_debug("%s: sched to a remote thread (%p), sched again\n", 
            __func__, target);
        sched();
        return;
    }
#endif
    int is_fpu_owner;

    /* TS_INTER may be set in signal_notific */
    BUG_ON((target->thread_ctx->state != TS_WAITING)
           && (target->thread_ctx->state != TS_WAITING_IPC)
           && (target->thread_ctx->state != TS_INTER));

    /* Switch to itself? */
    BUG_ON(target == current_thread);

    /*
     * We need to ensure the target thread kstack is free
     * before switching to it.
     *
     * Otherwise, wrong cases may occur. For example:
     * Time-1: CPU-0: T1 sends ipc to T2 ->
     *                T2 is enqueued and T1 is current thread
     *
     * Time-2: CPU-1: T2 runs and returns to T1 ->
     *                T1's state may be TS_READY.
     *
     * Time-3: CPU-0: sched.c finds old thread (T1)'s state is
     *                TS_READY (triggers BUG_ON).
     *
     * Another example:
     * If T1 want to direct switch to T2.
     * The target (server) thread may be still executing after
     * IPC returns (a very small time window),
     * so we need to wait until its stack is free.
     */
    wait_for_kernel_stack(target);

    /*
     * Since the target thread is waiting or be set to TS_INTER in
     * signal_notific, its is_fpu_owner state will not change
     * or can only be changed to -1.
     */

    is_fpu_owner = target->thread_ctx->is_fpu_owner;
    // kinfo("[**][%s] set is_fpu_owner: %d\n", __func__, is_fpu_owner);

    if ((is_fpu_owner >= 0) && (is_fpu_owner != smp_get_cpu_id())) {
        /*
         * Slow path: if target thread is fpu_owner of some other CPUs,
         * local CPU cannot direct switch to it.
         */

        target->thread_ctx->state = TS_INTER;
        BUG_ON(sched_enqueue(target));

        sched();
    } else {
        /* Fast path: direct switch to target thread (without
         * scheduling). */

        /*
         * TODO: if disallow sched_to_thread in notification,
         * we can add BUG_ON(current_thread->thread_ctx->state !=
         * TS_WAITING_IPC) here and remove the below if statement.
         */

        /* If current thread has not been set to TS_WAITING_IPC,
         * put it into the ready queue before switching to
         * the target thread.
         */
        if (current_thread->thread_ctx->state != TS_WAITING_IPC)
            BUG_ON(sched_enqueue(current_thread));

        switch_to_thread(target);
    }

    eret_to_thread(switch_context());
    /* The control flow will never return back. */
}

/*
 * Switch vmspace and arch-related stuff
 * Return the context pointer which should be set to stack pointer register
 */
u64 switch_context(void)
{
    /* TODO: with IRQ disabled.
     * tmac: what if IRQ is not disabled? But directly resumes the execution
     * after interrupts.
     */
    struct thread *target_thread;
    struct thread_ctx *target_ctx;

    target_thread = current_thread;
    BUG_ON(!target_thread);
    BUG_ON(!target_thread->thread_ctx);

    target_ctx = target_thread->thread_ctx;

    if (target_thread->prev_thread == THREAD_ITSELF)
        return (u64)target_ctx;

#ifndef CHCORE_KERNEL_TEST
    BUG_ON(!target_thread->vmspace);
    /*
     * Recording the CPU the thread runs on: for TLB maintainence.
     * switch_context is always required for running a (new) thread.
     * So, we invoke record_running_cpu here.
     */
    record_history_cpu(target_thread->vmspace, smp_get_cpu_id());
    switch_thread_vmspace_to(target_thread);
#else /* CHCORE_KERNEL_TEST */
    /* TYPE_TESTS threads do not have vmspace. */
    if (target_thread->thread_ctx->type != TYPE_TESTS) {
        BUG_ON(!target_thread->vmspace);
        record_history_cpu(target_thread->vmspace, smp_get_cpu_id());
        switch_thread_vmspace_to(target_thread);
    }
#endif /* CHCORE_KERNEL_TEST */

    arch_switch_context(target_thread);

    return (u64)target_ctx;
}

/*
 * Return the CPU which the thread is binded to (FPU owner or CPU affinity).
 * Return NO_AFF if the thread can run on any CPU.
 */
s32 get_cpubind(struct thread *thread)
{
#if FPU_SAVING_MODE == LAZY_FPU_MODE
    s32 affinity, is_fpu_owner;
    u32 local_cpuid;

    local_cpuid = smp_get_cpu_id();
    affinity = thread->thread_ctx->affinity;
    is_fpu_owner = thread->thread_ctx->is_fpu_owner;
    // kinfo("[**][%s] set is_fpu_owner: %d\n", __func__, is_fpu_owner);

#ifdef DSM_ENABLED
    if (!is_local_cpu(affinity))
        return affinity;
#endif
    /*
     * If the thread is the FPU owner of current CPU core,
     * save_and_release_fpu() can make it become not a owner.
     * Therefore, it can be migrated.
     *
     * However, if thread is a FPU owner of some other CPU,
     * we cannot directly migrate it.
     */
    if (is_fpu_owner < 0) {
        return affinity;
    } else if (is_fpu_owner == local_cpuid) {
        if (affinity == local_cpuid || affinity == NO_AFF) {
            return cpuid_l2g(local_cpuid);
        } else {
            save_and_release_fpu(thread);
            return affinity;
        }
    } else {
        return cpuid_l2g(is_fpu_owner);
    }
#else
    return thread->thread_ctx->affinity;
#endif
}

/*
 * find_runnable_thread
 * ** Shoule hold a dedicated lock for the thread_list and this function can
 * only be called in the critical section!! ** Only the thread which kernel
 * state is free can be choosed
 */
struct thread *find_runnable_thread(struct list_head *thread_list)
{
    struct thread *thread;

    for_each_in_list (thread, struct thread, ready_queue_node, thread_list) {
        if (thread->thread_ctx->kernel_stack_state == KS_FREE
            || thread == current_thread) {
            return thread;
        }
    }
    return NULL;
}

/* Pending rescheduling will be done when the kernel returns to userspace */
void add_pending_resched(u32 cpuid)
{
    BUG_ON(cpuid >= PLAT_CPU_NUM);
    resched_bitmaps[smp_get_cpu_id()] |= BIT(cpuid);
}

void wait_for_kernel_stack(struct thread *thread)
{
    /*
     * Handle IPI tx while waiting to avoid deadlock.
     *
     * Deadlock example:
     * CPU 0: waiting for CPU 1 to release its kernel stack,
     *        cannot receive IPI as it is in kernel mode;
     * CPU 1: waiting for CPU 0 to finish an IPI tx,
     *        cannot release its kernel stack as it is
     *        executing on that stack.
     */
    while (thread->thread_ctx->kernel_stack_state != KS_FREE) {
        handle_ipi();
    }
}

/* Defined as an asm func. */
extern void __eret_to_thread(u64 sp);

void do_pending_resched(void)
{
    u32 cpuid;
    u32 local_cpuid = smp_get_cpu_id();
    bool local_cpu_need_resched = false;

    while (resched_bitmaps[local_cpuid]) {
        cpuid = bsr(resched_bitmaps[local_cpuid]);
        resched_bitmaps[local_cpuid] &= ~BIT(cpuid);
        if (cpuid != local_cpuid) {
            send_ipi(cpuid, IPI_RESCHED);
        } else {
            local_cpu_need_resched = true;
        }
    }

    if (local_cpu_need_resched) {
        sched();
        eret_to_thread(switch_context());
    }
}

void finish_switch(void)
{
    struct thread *prev_thread;

    prev_thread = current_thread->prev_thread;
    if ((prev_thread == THREAD_ITSELF) || (prev_thread == NULL))
        return;

    if (prev_thread->thread_ctx->type == TYPE_SHADOW
        && prev_thread->thread_ctx->state == TS_EXIT) {
        cap_free(prev_thread->cap_group, prev_thread->cap);
        current_thread->prev_thread = NULL;
        return;
    }

    /*
     * This flag is checked during IPC, notification, recycle ...
     * A fence to ensure everthing is done before setting the stack_state
     * to KS_FREE (see __eret_to_thread for aarch64).
     */
    // smp_mb();
    prev_thread->thread_ctx->kernel_stack_state = KS_FREE;

#ifdef CHCORE_KERNEL_RT
    /*
     * If a resched IPI is received during send_ipi(),
     * the local CPU will re-schedule
     */
    do_pending_resched();
#endif
}

void eret_to_thread(u64 sp)
{
#ifndef CHCORE_KERNEL_RT
    finish_switch();
#endif
    __eret_to_thread(sp);
}

/* SYSCALL functions */

void sys_yield(void)
{
    struct thread *thread = current_thread;
    BUG_ON(!thread);

    thread->thread_ctx->sc->budget = 0;
    sched();
    eret_to_thread(switch_context());
}

void sys_top(void)
{
    cur_sched_ops->sched_top();
}

#define DEFAULT_STACK_SIZE (8 << 20)
char idle_thread_stack[PLAT_CPU_NUM][DEFAULT_STACK_SIZE];

static void init_idle_threads(void)
{
    u32 i;
    const char *idle_name = "KERNEL-IDLE";
    u32 idle_name_len = strlen(idle_name);
    struct cap_group *idle_cap_group;
    struct vmspace *idle_vmspace;

    /* Create a fake idle cap group to store the name */
    idle_cap_group = kzalloc(sizeof(*idle_cap_group), __MT_PRIVATE__);
    idle_name_len = MIN(idle_name_len, MAX_GROUP_NAME_LEN);
    memcpy(idle_cap_group->cap_group_name, idle_name, idle_name_len);
    init_list_head(&idle_cap_group->thread_list);

    extern struct vmspace *create_idle_vmspace(void); // in
                                                      // arch/mm/vmspace.c
    idle_vmspace = create_idle_vmspace();

    for (i = 0; i < PLAT_CPU_NUM; i++) {
        idle_threads[i].thread_ctx = create_thread_ctx(TYPE_IDLE);
        BUG_ON(idle_threads[i].thread_ctx == NULL);

        init_thread_ctx(
                &idle_threads[i], 0, 0, IDLE_PRIO, TYPE_IDLE, cpuid_l2g(i));

        extern void idle_thread_routine(void); // in arch/sched/idle.S
        arch_idle_ctx_init(idle_threads[i].thread_ctx,
                           (u64)idle_thread_stack
                                   + (i + 1) * DEFAULT_STACK_SIZE,
                           idle_thread_routine);

        idle_threads[i].cap_group = idle_cap_group;
        idle_threads[i].vmspace = idle_vmspace;
        list_add(&idle_threads[i].node, &idle_cap_group->thread_list);
    }
    kdebug("Create %d idle threads.\n", i);
}

static void init_current_threads(void)
{
    int i;

    for (i = 0; i < PLAT_CPU_NUM; i++) {
        current_threads[i] = NULL;
    }
}

static void init_resched_bitmaps(void)
{
    memset(resched_bitmaps, 0, sizeof(resched_bitmaps));
}

int sched_init(struct sched_ops *sched_ops)
{
    BUG_ON(sched_ops == NULL);

    init_idle_threads();
    init_current_threads();
    init_resched_bitmaps();

    cur_sched_ops = sched_ops;
    cur_sched_ops->sched_init();

    return 0;
}

/* Performance syscall */
void sys_perf_null(void)
{
#ifdef NO_VMSWITCH
    current_thread->thread_ctx->sc->budget = DEFAULT_BUDGET;
    // sched_enqueue(target);
    // sched();
    // eret_to_thread(switch_context());
    eret_to_thread((u64)target->thread_ctx);
#elif defined(NORMAL)
    /* Set thread state */
    current_thread->thread_ctx->sc->budget = DEFAULT_BUDGET;
    BUG_ON(sched_enqueue(target));
    /* Reschedule */
    sched();
    eret_to_thread(switch_context());
#endif
}
