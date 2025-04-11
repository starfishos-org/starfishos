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
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
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

    list_append(&(thread->ready_queue_node),
                &(rr_ready_queue_meta[cpuid].queue_head));
    rr_ready_queue_meta[cpuid].queue_len++;

    return 0;
}

#ifdef DSM_ENABLED
int __rr_sched_enqueue_shared_machine(struct thread *thread, u32 m_id)
{
    struct shared_queue_meta *queue = &(rr_shared_queue[m_id]);
    // lock(&(queue->queue_lock));
    dsm_debug("%s: thread (%s, %p) -> machine %d\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread,
              m_id);
    list_append(&(thread->shared_queue_node),
                &(rr_shared_queue[m_id].queue_head));
    queue->queue_len++;
    // unlock(&(queue->queue_lock));
    return 0;
}

/* dequeue w/o lock */
int __rr_sched_dequeue_shared_machine(struct thread *thread, u32 m_id)
{
    struct shared_queue_meta *queue = &(rr_shared_queue[m_id]);
    // lock(&(queue->queue_lock));
    dsm_debug("%s: thread (%s, %p, ctx=%p)\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread,
              thread->thread_ctx);
    list_del(&(thread->shared_queue_node));
    queue->queue_len--;
    // unlock(&(queue->queue_lock));
    return 0;
}

int __rr_sched_enqueue_shared(struct thread *thread, u32 cpuid)
{
    struct shared_queue_meta *queue = &(rr_shared_queue[cpuid]);
    // lock(&(queue->queue_lock));
    dsm_debug("%s: thread (%s, %p) -> cpuid %d\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread,
              cpuid);
    list_append(&(thread->shared_queue_node),
                &(rr_shared_queue[cpuid].queue_head));
    queue->queue_len++;
    // unlock(&(queue->queue_lock));
    return 0;
}

/* dequeue w/o lock */
int __rr_sched_dequeue_shared(struct thread *thread, u32 cpuid)
{
    struct shared_queue_meta *queue = &(rr_shared_queue[cpuid]);
    // lock(&(queue->queue_lock));
    dsm_debug("%s: thread (%s, %p, ctx=%p)\n",
              __func__,
              thread->cap_group->cap_group_name,
              thread,
              thread->thread_ctx);
    list_del(&(thread->shared_queue_node));
    queue->queue_len--;
    // unlock(&(queue->queue_lock));
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
        save_and_release_fpu(thread);
#endif
    }

    gcpuid = affinitiy;
    m_id = cpuid_g2mid(gcpuid);
    dsm_debug("[%s] enqueue thread (%s, %p, affinity=%d) to cpuid %d machine %d\n",
                __func__,
                thread->cap_group->cap_group_name,
                thread,
                thread->thread_ctx->affinity,
                gcpuid,
                m_id);

    lock(&(rr_shared_queue[gcpuid].queue_lock));
    ret = __rr_sched_enqueue_shared(thread, gcpuid);
    unlock(&(rr_shared_queue[gcpuid].queue_lock));

    return ret;
}

/**
 * rr_sched_migrate_from_shared_queue -- check shared queue and migrate
 * scheduled thread to local queue
 */
int rr_sched_migrate_from_shared_queue()
{
    u32 gcpuid = 0, lcpuid = 0;
    struct thread *thread, *tmp;
    int ret = 0;

    /* Fast path: fast check queue_len */
    /* Mostly, the shared queue is empty */
    if (likely(rr_cur_shared_queue.queue_len == 0)) {
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

    if (rr_cur_shared_queue.queue_len == 0) {
        unlock(&(rr_cur_shared_queue.queue_lock));
        return ret;
    }

    for_each_in_list_safe (thread, tmp,
                      shared_queue_node,
                      &(rr_cur_shared_queue.queue_head)) {
        gcpuid = thread->thread_ctx->affinity;
        BUG_ON(cpuid_g2mid(gcpuid) != CUR_MACHINE_ID);
        lcpuid = cpuid_g2l(gcpuid);

        /* move thread from shared queue to local queue */
        ret = __rr_sched_dequeue_shared(thread, gcpuid);
        thread->thread_ctx->thread_exit_state = TE_RUNNING;
        thread->thread_ctx->state = TS_RUNNING;
        thread->thread_ctx->sc->budget = DEFAULT_BUDGET;

        lock(&(rr_ready_queue_meta[lcpuid].queue_lock));
        ret = __rr_sched_enqueue(thread, lcpuid);
        unlock(&(rr_ready_queue_meta[lcpuid].queue_lock));

        dsm_debug("find remote task(%s, %p), sched to %d\n",
                 thread->cap_group->cap_group_name,
                 thread,
                 lcpuid);
        // print_thread(thread);
    }

    unlock(&(rr_cur_shared_queue.queue_lock));
    return ret;
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
    if (unlikely(thread->thread_ctx->prio > MAX_PRIO))
        return -EINVAL;

#ifdef DSM_ENABLED
    if (unlikely(gcpuid < 0 || gcpuid >= CLUSTER_CPU_NUM)) {
#else
    if (unlikely(gcpuid < 0 || gcpuid >= PLAT_CPU_NUM)) {
#endif
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
        dsm_debug("[%s] enqueue thread (%s, %p, affinity=%d) to cpuid %d machine %d\n",
                    __func__,
                    thread->cap_group->cap_group_name,
                    thread,
                    thread->thread_ctx->affinity,
                    gcpuid,
                    m_id);
        lock(&(rr_shared_queue[gcpuid].queue_lock));
        ret = __rr_sched_enqueue_shared(thread, gcpuid);
        unlock(&(rr_shared_queue[gcpuid].queue_lock));
        return ret;
    }
#endif

    lcpuid = cpuid_g2l(gcpuid);
    lock(&(rr_ready_queue_meta[lcpuid].queue_lock));
    ret = __rr_sched_enqueue(thread, lcpuid);
    unlock(&(rr_ready_queue_meta[lcpuid].queue_lock));
    return ret;
}

/* dequeue w/o lock */
int __rr_sched_dequeue(struct thread *thread)
{
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
    list_del(&(thread->ready_queue_node));
    init_empty_node(&thread->ready_queue_node);
    rr_ready_queue_meta[thread->thread_ctx->cpuid].queue_len--;
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

    cpuid = thread->thread_ctx->cpuid;
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
                save_and_release_fpu(old);
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
            if (old->thread_ctx->sc->budget != 0) {
                switch_to_thread(old);
                return 0; /* no schedule needed */
            }
            rr_sched_refill_budget(old, DEFAULT_BUDGET);
            old->thread_ctx->state = TS_TO_SCHED;
            BUG_ON(rr_sched_enqueue(old) != 0);
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
    /* Initialize owned shared queues */
    for (i = 0; i < CLUSTER_MAX_CPU_NUM; i++) {
        init_list_head(&(rr_shared_queue[i].queue_head));
        lock_init(&(rr_shared_queue[i].queue_lock));
        rr_shared_queue[i].queue_len = 0;
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
