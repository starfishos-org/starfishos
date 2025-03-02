/* Scheduler related functions are implemented here */
#include <arch/machine/smp.h>
#include <common/errno.h>
#include <common/kprint.h>
#include <common/list.h>
#include <common/lock.h>
#include <common/macro.h>
#include <common/util.h>
#include <common/bitops.h>
#include <irq/irq.h>
#include <machine.h>
#include <mm/kmalloc.h>
#include <object/thread.h>
#include <sched/context.h>
#include <sched/sched.h>
#include <sched/fpu.h>

/*
 * Priority bitmap.
 * Record the priorities of the ready threads in a bitmap
 * so that we can find the ready thread with the highest priortiy in O(1) time.
 * Single bitmap for small PRIO_NUM and multi-level bitmap for large PRIO_NUM.
 */
#if PRIO_NUM <= 32
struct prio_bitmap {
    u32 bitmap;
};

static inline void prio_bitmap_init(struct prio_bitmap *bitmap)
{
    bitmap->bitmap = 0;
}

static inline void prio_bitmap_set(struct prio_bitmap *bitmap, u32 prio)
{
    bitmap->bitmap |= BIT(prio);
}

static inline void prio_bitmap_clear(struct prio_bitmap *bitmap, u32 prio)
{
    bitmap->bitmap &= ~BIT(prio);
}

static inline bool prio_bitmap_is_empty(struct prio_bitmap *bitmap)
{
    return bitmap->bitmap == 0;
}

static inline int get_highest_prio(struct prio_bitmap *bitmap)
{
    return bsr(bitmap->bitmap);
}
#elif PRIO_NUM <= 32 * 32
struct prio_bitmap {
    u32 bitmap_lvl0;
    u32 bitmap_lvl1[32];
};

static inline void prio_bitmap_init(struct prio_bitmap *bitmap)
{
    memset(bitmap, 0, sizeof(*bitmap));
}

static inline void prio_bitmap_set(struct prio_bitmap *bitmap, u32 prio)
{
    u32 index_lvl0 = prio >> 5;
    u32 index_lvl1 = prio & 0x1f;

    BUG_ON(index_lvl0 >= 32);

    bitmap->bitmap_lvl0 |= BIT(index_lvl0);
    bitmap->bitmap_lvl1[index_lvl0] |= BIT(index_lvl1);
}

static inline void prio_bitmap_clear(struct prio_bitmap *bitmap, u32 prio)
{
    u32 index_lvl0 = prio >> 5;
    u32 index_lvl1 = prio & 0x1f;

    BUG_ON(index_lvl0 >= 32);

    bitmap->bitmap_lvl1[index_lvl0] &= ~BIT(index_lvl1);
    if (bitmap->bitmap_lvl1[index_lvl0] == 0) {
        bitmap->bitmap_lvl0 &= ~BIT(index_lvl0);
    }
}

static inline bool prio_bitmap_is_empty(struct prio_bitmap *bitmap)
{
    return bitmap->bitmap_lvl0 == 0;
}

static inline u32 get_highest_prio(struct prio_bitmap *bitmap)
{
    u32 index_lvl0;
    u32 index_lvl1;

    index_lvl0 = bsr(bitmap->bitmap_lvl0);
    index_lvl1 = bsr(bitmap->bitmap_lvl1[index_lvl0]);

    return (index_lvl0 << 5) + index_lvl1;
}
#else
#error PRIO_NUM should not be larger than 1024
#endif

struct pbrr_ready_queue {
    struct list_head queues[PRIO_NUM];
    struct prio_bitmap bitmap;
    struct lock lock;
};

static struct pbrr_ready_queue pbrr_ready_queues[PLAT_CPU_NUM];

extern struct thread idle_threads[PLAT_CPU_NUM]; // in sched/sched.c

int pbrr_sched_enqueue(struct thread *thread)
{
    s32 cpubind;
    u32 cpuid, prio;
    struct pbrr_ready_queue *ready_queue;

    BUG_ON(thread == NULL);
    BUG_ON(thread->thread_ctx == NULL);

    /* Already in a ready queue */
    if (thread->thread_ctx->state == TS_READY) {
        return -EINVAL;
    }

    prio = thread->thread_ctx->prio;
    BUG_ON(prio >= PRIO_NUM);

    cpubind = get_cpubind(thread);
    cpuid = (cpubind == NO_AFF ? smp_get_cpu_id() : cpubind);

    thread->thread_ctx->cpuid = cpuid;
    thread->thread_ctx->state = TS_READY;

    ready_queue = &pbrr_ready_queues[cpuid];
    lock(&ready_queue->lock);
    list_append(&thread->ready_queue_node, &ready_queue->queues[prio]);
    prio_bitmap_set(&ready_queue->bitmap, prio);
    unlock(&ready_queue->lock);

#ifdef CHCORE_KERNEL_TEST
    if (thread->thread_ctx->type != TYPE_TESTS) {
        add_pending_resched(cpuid);
    }
#else
    add_pending_resched(cpuid);
#endif

    return 0;
}

static void __pbrr_sched_dequeue(struct thread *thread)
{
    u32 cpuid, prio;
    struct pbrr_ready_queue *ready_queue;

    cpuid = thread->thread_ctx->cpuid;
    prio = thread->thread_ctx->prio;
    ready_queue = &pbrr_ready_queues[cpuid];

    thread->thread_ctx->state = TS_INTER;
    list_del(&thread->ready_queue_node);
    if (list_empty(&ready_queue->queues[prio])) {
        prio_bitmap_clear(&ready_queue->bitmap, prio);
    }
}

int pbrr_sched_dequeue(struct thread *thread)
{
    return -1; // unused
}

static struct thread *pbrr_sched_choose_thread(void)
{
    u32 cpuid, highest_prio;
    struct thread *thread;
    struct pbrr_ready_queue *ready_queue;
    bool current_thread_runnable;

    cpuid = smp_get_cpu_id();
    ready_queue = &pbrr_ready_queues[cpuid];

    thread = current_thread;
    current_thread_runnable = thread != NULL
                              && thread->thread_ctx->state == TS_RUNNING
                              && (thread->thread_ctx->affinity == NO_AFF
                                  || thread->thread_ctx->affinity == cpuid);

    lock(&ready_queue->lock);

retry:
    thread = current_thread;

    /* Choose current_thread if there is no other thread */
    if (prio_bitmap_is_empty(&ready_queue->bitmap)) {
        BUG_ON(thread->thread_ctx->type != TYPE_IDLE);
        BUG_ON(!current_thread_runnable);
        goto out_unlock_ready_queue;
    }

    highest_prio = get_highest_prio(&ready_queue->bitmap);

    /* Check whether we should choose current_thread */
    if (current_thread_runnable
        && (thread->thread_ctx->prio > highest_prio
            || (thread->thread_ctx->prio == highest_prio
                && thread->thread_ctx->sc->budget > 0))) {
        goto out_unlock_ready_queue;
    }

    /*
     * If the thread is just moved from another CPU and
     * the kernel stack is still used by the original CPU,
     * just choose the idle thread.
     *
     * We assume that thread moving between CPUs is rare
     * in realtime system, because users usually set the
     * CPU affinity of the threads.
     */
    thread = find_runnable_thread(&ready_queue->queues[highest_prio]);
    if (thread == NULL) {
        thread = &idle_threads[cpuid];
        if (thread == current_thread) {
            BUG_ON(!current_thread_runnable);
            goto out_unlock_ready_queue;
        }
    }

    __pbrr_sched_dequeue(thread);

    /* If the thread is going to exit, choose another thread. */
    if (thread->thread_ctx->thread_exit_state == TE_EXITING) {
        thread->thread_ctx->thread_exit_state = TE_EXITED;
        thread->thread_ctx->state = TS_EXIT;
        goto retry;
    }

out_unlock_ready_queue:
    unlock(&ready_queue->lock);
    return thread;
}

void pbrr_top(void)
{
    printk("[TODO] Implement pbrr_top()\n");
}

int pbrr_sched(void)
{
    struct thread *old, *new;

    old = current_thread;

    /* Check whether the old thread is going to exit */
    if (old != NULL && old->thread_ctx->thread_exit_state == TE_EXITING) {
        old->thread_ctx->thread_exit_state = TE_EXITED;
        old->thread_ctx->state = TS_EXIT;
    }

    new = pbrr_sched_choose_thread();
    BUG_ON(new == NULL);

    if (old != NULL && old->thread_ctx->state == TS_RUNNING && new != old) {
        BUG_ON(pbrr_sched_enqueue(old));
    }

    if (new->thread_ctx->sc->budget == 0) {
        new->thread_ctx->sc->budget = DEFAULT_BUDGET;
    }

    switch_to_thread(new);

    return 0;
}

int pbrr_sched_init(void)
{
    u32 i, j;

    /* Initialize the ready queues */
    for (i = 0; i < PLAT_CPU_NUM; i++) {
        prio_bitmap_init(&pbrr_ready_queues[i].bitmap);
        lock_init(&pbrr_ready_queues[i].lock);
        for (j = 0; j < PRIO_NUM; j++) {
            init_list_head(&pbrr_ready_queues[i].queues[j]);
        }
    }

    /* Insert the idle threads into the ready queues */
    for (i = 0; i < PLAT_CPU_NUM; i++) {
        pbrr_sched_enqueue(&idle_threads[i]);
    }

    return 0;
}

struct sched_ops pbrr = {.sched_init = pbrr_sched_init,
                         .sched = pbrr_sched,
                         .sched_enqueue = pbrr_sched_enqueue,
                         .sched_dequeue = pbrr_sched_dequeue,
                         .sched_top = pbrr_top};
