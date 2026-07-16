/* Scheduler related functions are implemented here */
#include <sched/sched.h>
#include <sched/fpu.h>
#include <arch/machine/smp.h>
#include <common/kprint.h>
#include <machine.h>
#include <mm/kmalloc.h>
#include <common/list.h>
#include <common/util.h>
#include <object/thread.h>
#include <common/macro.h>
#include <common/errno.h>
#include <common/types.h>
#include <common/lock.h>
#include <object/thread.h>
#include <irq/irq.h>
#include <sched/context.h>
#include <sched/sched.h>
#include <arch/time.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#include <drivers/ivshmem.h>
#include <arch/sync.h>
#define rr_shared_queue     (dsm_meta->shared_queue)
#define rr_cur_shared_queue (dsm_meta->shared_queue[cpuid_l2g(smp_get_cpu_id())])
#endif

/* in arch/sched/idle.S */
void idle_thread_routine(void);

/* Metadata for ready queue */
struct queue_meta {
    struct list_head queue_head;
    u32 queue_len;
    struct lock queue_lock;
    char pad[pad_to_cache_line(sizeof(u32) + sizeof(struct list_head)
                               + sizeof(struct lock))];
};

/*
 * rr_ready_queue
 * Per-CPU ready queue for ready tasks.
 */
struct queue_meta rr_ready_queue_meta[PLAT_CPU_NUM];

/*
 * RR policy also has idle threads.
 * When no active user threads in ready queue,
 * we will choose the idle thread to execute.
 * Idle thread will **NOT** be in the RQ.
 */
extern struct thread idle_threads[PLAT_CPU_NUM]; // in sched/sched.c

int __rr_sched_enqueue(struct thread *thread, u32 cpuid)
{
    /* should not enqueue local queue */
    BUG_ON(!is_local_cpu(thread->thread_ctx->affinity));

    /* Already in the ready queue */
    if (thread->thread_ctx->state == TS_READY) {
        return -EINVAL;
    }
    thread->thread_ctx->cpuid = cpuid;
    thread->thread_ctx->state = TS_READY;

    if (!list_empty(&thread->ready_queue_node)) {
        kwarn("%s: thread %p is in ready queue\n", __func__, thread);
        // print_thread(thread);
    }

    /* Record which CPU queue this thread is enqueued in */
    thread->queue_cpuid = cpuid;

    list_append(&(thread->ready_queue_node),
                &(rr_ready_queue_meta[cpuid].queue_head));
    rr_ready_queue_meta[cpuid].queue_len++;

    return 0;
}

#ifdef DSM_ENABLED
int __rr_sched_enqueue_shared_machine(struct thread *thread, u32 m_id)
{
    dsm_debug("%s: thread (%s, %p) -> machine %d\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread,
              m_id);
    thread_dq_enqueue(&(rr_shared_queue[m_id]), thread);
    return 0;
}

int __rr_sched_enqueue_shared(struct thread *thread, u32 cpuid)
{
    // should check that the thread is ready to run across machines
    // extern int check_thread_ready_to_run_across_machines(struct thread *thread);
    // BUG_ON(check_thread_ready_to_run_across_machines(thread));

    dsm_debug("%s: thread (%s, %p) -> cpuid %d\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread,
              cpuid);
    thread_dq_enqueue(&(rr_shared_queue[cpuid]), thread);
    return 0;
}

/**
 * rr_sched_migrate_to_remote -- migrate thread to remote
 * @thread: thread to be migrated
 */
int rr_sched_migrate_to_remote(struct thread *thread)
{
    u64 affinitiy, gcpuid, m_id;
    int ret;

    (void)m_id;

    /* remote sched has the highest prio */
    BUG_ON(!thread);
    BUG_ON(!thread->thread_ctx);

    affinitiy = thread->thread_ctx->affinity;
    BUG_ON(is_local_cpu(affinitiy));

    if (thread->thread_ctx->is_fpu_owner >= 0) {
        dsm_debug(
                "%s: save and release fpu of thread (%p)\n", __func__, thread);
#if FPU_SAVING_MODE == LAZY_FPU_MODE
        /* sys_set_aff -> sched -> save_and_release_fpu */
        save_and_release_fpu(thread, smp_get_cpu_id());
#endif
    }

    gcpuid = affinitiy;
    m_id = cpuid_g2mid(gcpuid);
    dsm_debug("[%s:%d] enqueue thread (%s, %p, affinity=%d) to cpuid %d machine %d\n",
                __FILE__,
                __LINE__,
                thread->cap_group->cap_group_name,
                thread,
                thread->thread_ctx->affinity,
                gcpuid,
                m_id);

    lock(&(rr_shared_queue[gcpuid].queue_lock));
    ret = __rr_sched_enqueue_shared(thread, gcpuid);
    unlock(&(rr_shared_queue[gcpuid].queue_lock));

    /* The migrated thread is still current on the source CPU.  Defer the
     * doorbell until finish_switch() has released it. */
    if (ret == 0)
        sched_defer_remote_kick(gcpuid);

    return ret;
}

void copy_thread_ctx_to_dst(struct thread_ctx *src, struct thread_ctx *dst)
{
    memcpy(dst, src, sizeof(struct thread_ctx));
    dst->fpu_state = kzalloc(STATE_AREA_SIZE, __MT_PRIVATE__);
    memcpy(dst->fpu_state, src->fpu_state, STATE_AREA_SIZE);
    dst->sc = kzalloc(sizeof(sched_cont_t), __MT_PRIVATE__);
    memcpy(dst->sc, src->sc, sizeof(sched_cont_t));
}

/**
 * rr_sched_migrate_from_shared_queue -- check shared queue and migrate
 * scheduled thread to local queue
 */
static int rr_sched_migrate_from_shared_queue_internal(bool urgent)
{
    u32 gcpuid = 0, lcpuid = 0;
    struct thread *thread;
    struct list_head *urgent_tail = NULL;
    int ret = 0;

    /* Fast path: check if empty (no lock needed for lockless dq) */
    /* Mostly, the shared queue is empty */
    if (likely(rr_cur_shared_queue.head == rr_cur_shared_queue.tail)) {
        return 0;
    }

    /* check shared queue of current machine */
    /*
     * FIXME(PERFORMANCE):
     * this might be very time consuming, lock a shared lock
     * need a fast path to check whether there is thread need to
     * migrate
     */
    lock(&(rr_cur_shared_queue.queue_lock));

    if (rr_cur_shared_queue.head == rr_cur_shared_queue.tail) {
        unlock(&(rr_cur_shared_queue.queue_lock));
        return ret;
    }

    while ((thread = thread_dq_dequeue(&rr_cur_shared_queue)) != NULL) {
        // measure dequeue shared & enqueue local
        // u64 begin = plat_get_mono_time();
        gcpuid = thread->thread_ctx->affinity;
        // BUG_ON(cpuid_g2mid(gcpuid) == CUR_MACHINE_ID);
        lcpuid = cpuid_g2l(gcpuid);

        thread->thread_ctx->thread_exit_state = TE_RUNNING;
        thread->thread_ctx->state = TS_RUNNING;
        thread->thread_ctx->sc->budget = DEFAULT_BUDGET;
        thread->machine_id = CUR_MACHINE_ID;

        /* CRITICAL: Reset kernel_stack_state when migrating thread to local machine.
         * The thread was running on another machine, but now it's migrated to
         * the current machine. The kernel_stack_state from the old machine is
         * no longer valid, so we can safely reset it to KS_FREE. */
        thread->thread_ctx->kernel_stack_state = KS_FREE;

#ifdef PHOENIX_SCHED_TIMING
        {
            u64 arrive_tsc = dsm_tsc_to_m0(get_cycles());
            /* Never print from this wake-up path: synchronous serial output
             * is part of the measured end-to-end latency under QEMU. Keep the
             * calculation available for debugger/perf-counter inspection. */
            (void)(arrive_tsc - thread->thread_ctx->migrate_tsc);
        }
#endif

        // copy thread_ctx to local thread_ctx (for paper) measure copy ctx to local
        // struct thread_ctx *local_thread_ctx_on_dram = kzalloc(sizeof(struct thread_ctx), __MT_SHARED__);
        // copy_thread_ctx_to_dst(thread->thread_ctx, local_thread_ctx_on_dram);
        // thread->thread_ctx = local_thread_ctx_on_dram;

        lock(&(rr_ready_queue_meta[lcpuid].queue_lock));
        ret = __rr_sched_enqueue(thread, lcpuid);
        if (ret == 0 && urgent) {
            /* Keep remote FIFO order, but place this wake batch ahead of
             * unrelated local runnable work. */
            list_del(&thread->ready_queue_node);
            if (!urgent_tail)
                urgent_tail = &rr_ready_queue_meta[lcpuid].queue_head;
            list_add(&thread->ready_queue_node, urgent_tail);
            urgent_tail = &thread->ready_queue_node;
        }
        unlock(&(rr_ready_queue_meta[lcpuid].queue_lock));

        // u64 end = plat_get_mono_time();
        // printk("migrate to local time: %llu\n", end - begin);

        dsm_debug("find remote task(%s, %p), sched to %d\n",
                 thread->cap_group->cap_group_name,
                 thread,
                 lcpuid);
    }

    unlock(&(rr_cur_shared_queue.queue_lock));
    return ret;
}

int rr_sched_migrate_from_shared_queue(void)
{
    return rr_sched_migrate_from_shared_queue_internal(false);
}

int rr_sched_migrate_from_shared_queue_urgent(void)
{
    return rr_sched_migrate_from_shared_queue_internal(true);
}

bool rr_sched_remote_queue_pending(void)
{
    smp_mb();
    return rr_cur_shared_queue.head != rr_cur_shared_queue.tail;
}
#endif

/* FIXME: A dummy config, no optimized for performance */
#define LOADBALANCE_THRESHOLD 0
#define MIGRATE_THRESHOLD     0

/* A simple load balance when enqueue threads */
static u32 rr_sched_choose_cpu(void)
{
    u32 i, cpuid, min_rr_len, local_cpuid, queue_len;
    u32 j, start;

    local_cpuid = smp_get_cpu_id();
    min_rr_len = rr_ready_queue_meta[local_cpuid].queue_len;

    if (min_rr_len <= LOADBALANCE_THRESHOLD) {
        return cpuid_l2g(local_cpuid);
    }

    /* Find the cpu with the shortest ready queue */
    cpuid = local_cpuid;
    start = (cpuid + 1) % PLAT_CPU_NUM;
    for (j = 0; j < PLAT_CPU_NUM; j++) {
        i = (start + j) % PLAT_CPU_NUM;
        if (i == local_cpuid) {
            continue;
        }

        queue_len = rr_ready_queue_meta[i].queue_len + MIGRATE_THRESHOLD;
        if (queue_len < min_rr_len) {
            min_rr_len = queue_len;
            cpuid = i;
        }
    }

    return cpuid_l2g(cpuid);
}

/**
 * @brief Wake up a thread on another machine
 * 
 * @param bridge_thread: link to remote thread
 * @return success: 0, failed: -1
 */
int __get_bridge_thread_gcpuid(struct object *bridge_thread_object)
{
    BUG_ON(bridge_thread_object->type != TYPE_THREAD);
    BUG_ON(bridge_thread_object->mem_type != __MT_SHARED__);
    BUG_ON(bridge_thread_object->dsm_type != DSM_TYPE_THREAD_NOTIFY_BRIDGE);

    struct thread *bridge_thread = 
        (struct thread *)object2obj(bridge_thread_object);

    /**
     * Know which gcpuid to enqueue
     * Case1: remote thread has a cpuid affinity
     * Case2: remote thread has no cpuid affinity
     * However, we can not know this from the bridge thread
     * so we left it for the target machine to check again
     * 
     * refer to dsm_copy_notification() in dsm_objects/notification.c
     * cpuid here is a cached local cpuid, might differ from the
     * actual cpuid of the thread on the target machine
     */
    if (bridge_thread->thread_ctx->cpuid == NO_AFF) {
        return bridge_thread->machine_id;
    } else {
        // The local 0 cpu on machine_id
        return cpuid_l2g_with_mid(bridge_thread->thread_ctx->cpuid,
                             bridge_thread->machine_id);
    }
}

/*
 * Sched_enqueue
 * Put `thread` at the end of ready queue of assigned `affinity` and `prio`.
 * If affinity = NO_AFF, assign the core to the current cpu.
 * If the thread is IDEL thread, do nothing!
 */
int rr_sched_enqueue(struct thread *thread)
{
    BUG_ON(!thread);
    BUG_ON(!thread->thread_ctx);

    s32 cpubind = 0;
    u32 gcpuid = 0, lcpuid;
    int ret = 0;
    int m_id;

    (void)m_id;

    if (thread->thread_ctx->type == TYPE_IDLE)
        return 0;

    if (thread->thread_ctx->thread_exit_state == TE_EXITING) {
        thread->thread_ctx->thread_exit_state = TE_EXITED;
        return 0;
    }

    if (thread->thread_ctx->thread_exit_state == TE_STOPPING) {
        thread->thread_ctx->thread_exit_state = TE_STOPPED;
        return 0;
    }

#ifdef DSM_ENABLED
    if (unlikely(obj2object(thread)->dsm_type == DSM_TYPE_THREAD_NOTIFY_BRIDGE)) {
        gcpuid = __get_bridge_thread_gcpuid(obj2object(thread));
    } else {
        cpubind = get_cpubind(thread);
        gcpuid = cpubind == NO_AFF ? rr_sched_choose_cpu() : cpubind;
    }
#else
    cpubind = get_cpubind(thread);
    gcpuid = cpubind == NO_AFF ? rr_sched_choose_cpu() : cpubind;
#endif

    /* Check Prio */
    if (unlikely(thread->thread_ctx->prio > MAX_PRIO)) {
        kwarn("thread %s prio %d is out of range\n",
              thread->cap_group->cap_group_name,
              thread->thread_ctx->prio);
        return -EINVAL;
    }

#ifdef DSM_ENABLED
    if (unlikely(gcpuid < 0 || gcpuid >= CLUSTER_CPU_NUM)) {
#else
    if (unlikely(gcpuid < 0 || gcpuid >= PLAT_CPU_NUM)) {
#endif
        kwarn("thread %s gcpuid %d is out of range\n",
              thread->cap_group->cap_group_name,
              gcpuid);
        return -EINVAL;
    }

#ifdef DSM_ENABLED
    /* enqueue should only be called by a local sched */
    if (!is_local_cpu(gcpuid)) {
        /* enqueue a alreay migrated thread */
        // dsm_debug("enqueue a already migrated thread (%p, ctx=%p) to
        // gcpuid=%d\n",
        //  thread, thread->thread_ctx, gcpuid);
        // print_thread(thread);
        m_id = cpuid_g2mid(gcpuid);
        dsm_debug("[%s:%d] enqueue thread (%s, %p, affinity=%d, current_cpu=%d, machine_id=%d) to cpuid %d machine %d\n",
                    __FILE__,
                    __LINE__,
                    thread->cap_group->cap_group_name,
                    thread,
                    thread->thread_ctx->affinity,
                    thread->thread_ctx->cpuid,
                    thread->machine_id,
                    gcpuid,
                    m_id);
        // measure enqueue shared
        // u64 begin = plat_get_mono_time();
        lock(&(rr_shared_queue[gcpuid].queue_lock));
        ret = __rr_sched_enqueue_shared(thread, gcpuid);
        unlock(&(rr_shared_queue[gcpuid].queue_lock));
        // u64 end = plat_get_mono_time();
        // printk("enqueue shared time: %llu\n", end - begin);
        if (ret == 0) {
            if (thread == current_thread)
                sched_defer_remote_kick(gcpuid);
            else if (ivshmem_send_sched_msi(gcpuid) != 0)
                kwarn_once("Remote scheduler MSI failed; using tick fallback\n");
        }
        return ret;
    }
#endif

    lcpuid = cpuid_g2l(gcpuid);
    lock(&(rr_ready_queue_meta[lcpuid].queue_lock));
    ret = __rr_sched_enqueue(thread, lcpuid);
    unlock(&(rr_ready_queue_meta[lcpuid].queue_lock));

    /* Wake an idle remote CPU immediately instead of waiting for its next
     * scheduler tick.  A running thread migrating itself must defer the kick
     * until the source CPU has finished switching away from it. */
    if (ret == 0 && lcpuid != smp_get_cpu_id()) {
        if (thread == current_thread)
            add_pending_resched(lcpuid);
        else
            sched_kick_cpu(lcpuid);
    }
    return ret;
}

#ifdef DSM_ENABLED
/**
 * Enqueue thread to its affinity queue. If affinity is on another machine,
 * enqueue to that machine's shared queue (so the thread is woken on the
 * correct machine). Used by notification/timer wake-up when the woken thread
 * belongs to a remote machine.
 */
int rr_sched_enqueue_to_affinity(struct thread *thread)
{
    s32 aff;
    u32 gcpuid;

    BUG_ON(!thread || !thread->thread_ctx);
    if (thread->thread_ctx->type == TYPE_IDLE)
        return 0;
    if (thread->thread_ctx->thread_exit_state == TE_EXITING) {
        thread->thread_ctx->thread_exit_state = TE_EXITED;
        return 0;
    }
    if (thread->thread_ctx->thread_exit_state == TE_STOPPING) {
        thread->thread_ctx->thread_exit_state = TE_STOPPED;
        return 0;
    }

    aff = get_cpubind(thread);
    if (aff == NO_AFF || is_local_cpu(aff)) {
        /* Local or no affinity: use normal enqueue (will choose CPU if NO_AFF) */
        return rr_sched_enqueue(thread);
    }

    gcpuid = (u32)aff;
    lock(&(rr_shared_queue[gcpuid].queue_lock));
    thread->thread_ctx->state = TS_READY;
    thread_dq_enqueue(&(rr_shared_queue[gcpuid]), thread);
    unlock(&(rr_shared_queue[gcpuid].queue_lock));
    if (ivshmem_send_sched_msi(gcpuid) != 0)
        kwarn_once("Remote scheduler MSI failed; using tick fallback\n");
    return 0;
}
#endif

/* dequeue w/o lock */
int __rr_sched_dequeue(struct thread *thread)
{
    u32 cpuid;
    
    if (unlikely(thread->thread_ctx->state == TS_RUNNING ||
                thread->thread_ctx->state == TS_WAITING ||
                thread->thread_ctx->state == TS_WAITING_IPC)) {
        kinfo("%s: thread %s state is %d exit state is %d\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread->thread_ctx->state,
              thread->thread_ctx->thread_exit_state);
        return -EINVAL;
    }
    
    /* Use the recorded queue_cpuid to ensure consistency */
    cpuid = thread->queue_cpuid;
    /* If queue_cpuid is NO_AFF, thread is already dequeued */
    if (cpuid == NO_AFF) {
        return 0; /* Already dequeued, return success */
    }
    
    list_del(&(thread->ready_queue_node));
    init_empty_node(&thread->ready_queue_node);
    rr_ready_queue_meta[cpuid].queue_len--;
    
    /* Clear queue_cpuid after dequeue */
    thread->queue_cpuid = NO_AFF; /* Mark as invalid */
    return 0;
}

/*
 * remove `thread` from its current residual ready queue
 */
int rr_sched_dequeue(struct thread *thread)
{
    BUG_ON(!thread);
    BUG_ON(!thread->thread_ctx);
    /* IDLE thread will **not** be in any ready queue */
    BUG_ON(thread->thread_ctx->type == TYPE_IDLE);

    u32 cpuid = 0;
    int ret = 0;

    /* Use queue_cpuid to get the correct lock */
    cpuid = thread->queue_cpuid;
    /* If queue_cpuid is NO_AFF, thread is already dequeued */
    if (cpuid == NO_AFF) {
        return 0; /* Already dequeued, return success */
    }
    lock(&(rr_ready_queue_meta[cpuid].queue_lock));
    ret = __rr_sched_dequeue(thread);
    thread->thread_ctx->state = TS_INTER;
    unlock(&(rr_ready_queue_meta[cpuid].queue_lock));
    return ret;
}

/*
 * Choose an appropriate thread and dequeue from ready queue
 */
struct thread *rr_sched_choose_thread(void)
{
    u32 cpuid = smp_get_cpu_id();
    struct thread *thread = NULL;
    int ret = 0;

    if (!list_empty(&(rr_ready_queue_meta[cpuid].queue_head))) {
        lock(&(rr_ready_queue_meta[cpuid].queue_lock));
    again:
        if (list_empty(&(rr_ready_queue_meta[cpuid].queue_head))) {
            unlock(&(rr_ready_queue_meta[cpuid].queue_lock));
            goto out;
        }
        /*
         * When the thread is just moved from another cpu and
         * the kernel stack is used by the origina core, try
         * to find another thread.
         */
        thread = find_runnable_thread(&(rr_ready_queue_meta[cpuid].queue_head));

        if (unlikely(!thread)) {
            unlock(&(rr_ready_queue_meta[cpuid].queue_lock));
            goto out;
        }

        BUG_ON(__rr_sched_dequeue(thread));
        if (unlikely(thread->thread_ctx->thread_exit_state == TE_EXITING)) {
            thread->thread_ctx->state = TS_EXIT;
            thread->thread_ctx->thread_exit_state = TE_EXITED;
            goto again;
        }

        thread->thread_ctx->state = TS_TO_SCHED; // serve as lock of the thread
        // someone else stop this thread
        if (unlikely(thread->thread_ctx->thread_exit_state == TE_STOPPING)) {
            thread->thread_ctx->thread_exit_state = TE_STOPPED;
            goto again;
        }

        thread->thread_ctx->state = TS_TO_SCHED;
        if (ret < 0) { /* thread is stopped by recycler or ckpt/restore */
            goto again;
        }

        unlock(&(rr_ready_queue_meta[cpuid].queue_lock));
        return thread;
    }
out:
    return &idle_threads[cpuid];
}

static inline void rr_sched_refill_budget(struct thread *target, u32 budget)
{
    target->thread_ctx->sc->budget = budget;
}

/*
 * Schedule a thread to execute.
 * current_thread can be NULL, or the state is TS_RUNNING or TS_WAITING.
 * This function will suspend current running thread, if any, and schedule
 * another thread from `(rr_ready_queue_meta[cpuid].queue_head)`.
 * ***the following text might be outdated***
 * 1. Choose an appropriate thread through calling *chooseThread* (Simple
 * Priority-Based Policy)
 * 2. Update current running thread and left the caller to restore the executing
 * context
 */
int rr_sched(void)
{
    /* WITH IRQ Disabled */
    struct thread *old = current_thread;
    struct thread *new = 0;

    if (old) {
        BUG_ON(!old->thread_ctx);

        /* old thread may pass its scheduling context to others. */
        if (old->thread_ctx->type != TYPE_SHADOW
            && old->thread_ctx->type != TYPE_REGISTER) {
            BUG_ON(!old->thread_ctx->sc);
        }

        /* Update scheduling history for old thread */
        u32 current_cpu = cpuid_l2g(smp_get_cpu_id());
        /* Only record if the current CPU is different from the previous one */
        if (old->sched_history.history_count == 0 || 
            old->sched_history.cpu_history[(old->sched_history.history_index + MAX_SCHED_HISTORY - 1) % MAX_SCHED_HISTORY] != current_cpu) {
            old->sched_history.cpu_history[old->sched_history.history_index] = current_cpu;
            old->sched_history.history_index = (old->sched_history.history_index + 1) % MAX_SCHED_HISTORY;
            if (old->sched_history.history_count < MAX_SCHED_HISTORY) {
                old->sched_history.history_count++;
            }
        }

        switch (old->thread_ctx->thread_exit_state) {
        case TE_RUNNING:
            break;
        case TE_EXITED:
            BUG_ON(1);
        case TE_EXITING:
            /**
             * Set TE_EXITING after check won't cause any trouble, the
             * thread will be recycle afterwards. Just a fast path.
             * Check whether the thread is going to exit.
             */
            old->thread_ctx->state = TS_EXIT;
            kdebug("%s: thread %s exit\n", old->cap_group->cap_group_name, __func__);
            old->thread_ctx->kernel_stack_state = KS_FREE;
            old->thread_ctx->thread_exit_state = TE_EXITED;
            BUG_ON(rr_sched_dequeue(old) != 0);
            old->thread_ctx->state = TS_EXIT;
            goto out;
#ifdef DSM_ENABLED
        case TE_MIGRATING:
            /* schedule migrate thread to remote */
            rr_sched_migrate_to_remote(old);
            goto out;
        case TE_STOPPING:
            old->thread_ctx->thread_exit_state = TE_STOPPED;
            /* DO NOT break, and follow to next case */
        case TE_STOPPED:
#if FPU_SAVING_MODE == LAZY_FPU_MODE
            if (old->thread_ctx->is_fpu_owner >= 0) {
                save_and_release_fpu(old, smp_get_cpu_id());
            }
#else
            save_fpu_state(old);
#endif
            goto out;
#endif
        default:
            BUG("Unexpected thread exit state: %d", old->thread_ctx->thread_exit_state);
        }

        /* check old state */
        switch (old->thread_ctx->state) {
        case TS_EXIT:
            /* do nothing */
            break;
        case TS_RUNNING:
            /* A thread without SC should not be TS_RUNNING. */
            BUG_ON(!old->thread_ctx->sc);
            // if (old->machine_id != CUR_MACHINE_ID) {
                // kwarn_once("[%s] thread %s (%p, cpuid=%d, affinity=%d) is not on current machine\n",
                //             __func__,
                //             old->cap_group->cap_group_name,
                //             old,
                //             old->thread_ctx->cpuid,
                //             old->thread_ctx->affinity);
                // return 0;
            // }
            if (old->thread_ctx->sc->budget != 0) {
                switch_to_thread(old);
                return 0; /* no schedule needed */
            }
            rr_sched_refill_budget(old, DEFAULT_BUDGET);
            old->thread_ctx->state = TS_TO_SCHED;
            if (rr_sched_enqueue(old) != 0) {
                print_thread(old);
                BUG("failed to enqueue thread: %s\n", old->cap_group->cap_group_name);
            }
            break;
        case TS_WAITING:
        case TS_WAITING_IPC:
            /* do nothing */
            break;
        default:
            kinfo("thread state: %d\n", old->thread_ctx->state);
            BUG_ON(1);
            break;
        }
    }

out:
#ifdef DSM_ENABLED
    rr_sched_migrate_from_shared_queue();
#endif

    new = rr_sched_choose_thread();
    BUG_ON(!new);
    switch_to_thread(new);

    return 0;
}

int rr_sched_init(void)
{
    int i = 0;

    /* Initialize global variables */
    for (i = 0; i < PLAT_CPU_NUM; i++) {
        init_list_head(&(rr_ready_queue_meta[i].queue_head));
        lock_init(&(rr_ready_queue_meta[i].queue_lock));
        rr_ready_queue_meta[i].queue_len = 0;
    }

#ifdef DSM_ENABLED
    /*
     * The node pool and queue sentinels are shared by the whole cluster.
     * Initializing the queues on every machine consumes 1024 sentinels per
     * machine and exhausts the 4095-node free list on machine 3.  Publish the
     * complete structure once, after every queue head and lock is ready.
     */
    if (CUR_MACHINE_ID == 0) {
        /* sched_init() is also called by the runtime shutdown/restore path.
         * Do not rebuild a live cluster-wide allocator in that case.  The
         * full-cluster AE cold-boot path (which assumes no live peers)
         * explicitly invalidates READY before arriving here. */
        if (__atomic_load_n(&dsm_meta->thread_dq_pool.init_state,
                            __ATOMIC_ACQUIRE)
            != THREAD_DQ_POOL_READY) {
            __atomic_store_n(&dsm_meta->thread_dq_pool.init_state,
                             THREAD_DQ_POOL_INITIALIZING,
                             __ATOMIC_RELAXED);
            thread_dq_pool_init();

            for (i = 0; i < CLUSTER_MAX_CPU_NUM; i++) {
                if (thread_dq_init(&(rr_shared_queue[i])) != 0) {
                    kwarn("Failed to initialize shared queue %d\n", i);
                    return -ENOMEM;
                }
            }

            __atomic_store_n(&dsm_meta->thread_dq_pool.init_state,
                             THREAD_DQ_POOL_READY,
                             __ATOMIC_RELEASE);
        }
    } else {
        while (__atomic_load_n(&dsm_meta->thread_dq_pool.init_state,
                               __ATOMIC_ACQUIRE)
               != THREAD_DQ_POOL_READY)
            CPU_PAUSE();
    }
#endif
    return 0;
}

#define MAX_CAP_GROUP_BUF 256

void rr_top(void)
{
    u32 cpuid;
    struct thread *thread;
    /* A simple way to collect all cap groups */
    struct cap_group *cap_group_buf[MAX_CAP_GROUP_BUF] = {0};
    unsigned int cap_group_num = 0;
    int i = 0;

    for (cpuid = 0; cpuid < PLAT_CPU_NUM; cpuid++) {
        lock(&(rr_ready_queue_meta[cpuid].queue_lock));
    }

    printk("\n*****CPU RQ Info*****\n");
    for (cpuid = 0; cpuid < PLAT_CPU_NUM; cpuid++) {
        printk("== CPU %d RQ LEN %lu==\n",
               cpuid,
               rr_ready_queue_meta[cpuid].queue_len);
        thread = current_threads[cpuid];
        if (thread != NULL) {
            for (i = 0; i < cap_group_num; i++)
                if (thread->cap_group == cap_group_buf[i])
                    break;
            if (i == cap_group_num && cap_group_num < MAX_CAP_GROUP_BUF) {
                cap_group_buf[cap_group_num] = thread->cap_group;
                cap_group_num++;
            }
            printk("Current ");
            print_thread(thread);
        }
        if (!list_empty(&(rr_ready_queue_meta[cpuid].queue_head))) {
            for_each_in_list (thread,
                              struct thread,
                              ready_queue_node,
                              &(rr_ready_queue_meta[cpuid].queue_head)) {
                for (i = 0; i < cap_group_num; i++)
                    if (thread->cap_group == cap_group_buf[i])
                        break;
                if (i == cap_group_num && cap_group_num < MAX_CAP_GROUP_BUF) {
                    cap_group_buf[cap_group_num] = thread->cap_group;
                    cap_group_num++;
                }
                print_thread(thread);
            }
        }
        printk("\n");
    }

    printk("\n*****CAP GROUP Info*****\n");
    for (i = 0; i < cap_group_num; i++) {
        printk("== CAP GROUP:%s ==\n", cap_group_buf[i]->cap_group_name);
        for_each_in_list (
                thread, struct thread, node, &(cap_group_buf[i]->thread_list)) {
            print_thread(thread);
        }
        printk("\n");
    }
    for (cpuid = 0; cpuid < PLAT_CPU_NUM; cpuid++) {
        unlock(&(rr_ready_queue_meta[cpuid].queue_lock));
    }
}

void rr_sched_clear()
{
    /* clear all queues */
    int i;
    for (i = 0; i < PLAT_CPU_NUM; i++) {
        lock(&(rr_ready_queue_meta[i].queue_lock));
        rr_ready_queue_meta[i].queue_len = 0;
        init_list_head(&(rr_ready_queue_meta[i].queue_head));
        unlock(&(rr_ready_queue_meta[i].queue_lock));
    }

    /* clear all current threads and mark resched */
    for (i = 0; i < PLAT_CPU_NUM; i++) {
        if (i == smp_get_cpu_id())
            continue;
        current_threads[i] = NULL;
        resched_flags[i] = true;
    }
    printk("clear sched finished\n");
}

struct sched_ops rr = {
        .sched_init = rr_sched_init,
        .sched = rr_sched,
        .sched_enqueue = rr_sched_enqueue,
        .sched_dequeue = rr_sched_dequeue,
        .sched_top = rr_top,
        .sched_clear = rr_sched_clear,
};
