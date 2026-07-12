#include <ipc/connection.h>
#include <ipc/notification.h>
#include <common/list.h>
#include <common/errno.h>
#include <object/thread.h>
#include <object/irq.h>
#include <sched/sched.h>
#include <sched/context.h>
#include <irq/irq.h>
#include <posix/time.h>
#include <mm/uaccess.h>
#include <ckpt/ckpt.h>

#ifdef DSM_ENABLED
#include <dsm/tiering.h>
#endif

void init_notific(struct notification *notifc)
{
    notifc->not_delivered_notifc_count = 0;
    notifc->waiting_threads_count = 0;
    thread_dq_init(&notifc->waiting_threads);
    lock_init(&notifc->notifc_lock);
    notifc->state = NOTIFIC_VALID;
}

void deinit_notific(struct notification *notifc)
{
        /* No deinitialization is required for now. */
}

void notification_deinit(void *ptr)
{
        deinit_notific((struct notification *)ptr);
}

/*
 * A waiting thread can be awoken by timeout and signal, which leads to racing.
 * We guarantee that a thread is not awoken for twice by 1. removing a thread
 *from notification waiting_threads when timeout and 2. removing a thread from
 * sleep_list when get signaled.
 * When signaled:
 *	lock(notification)
 *	remove from waiting_threads
 *      thread state = TS_READY
 *	unlock(notification)
 *
 *	if (sleep_state.cb != NULL) {
 *		lock(sleep_list)
 *		if (sleep_state.cb != NULL)
 *			remove from sleep_list
 *		unlock(sleep_list)
 *	}
 *
 * When timeout:
 *	lock(sleep_list)
 *	remove from sleep_list
 *	lock(notification)
 *	if (thread state == TS_WAITING)
 *		remove from waiting_threads
 *	unlock(notification)
 *	sleep_state.cb = NULL
 *	unlock(sleep_list)
 */

static void notific_timer_cb(struct thread *thread)
{
    struct notification *notifc;

    notifc = thread->sleep_state.pending_notific;
    thread->sleep_state.pending_notific = NULL;

    lock(&notifc->notifc_lock);

    /* For recycling: the state is set in stop_notification */
    if (notifc->state == NOTIFIC_INVALID) {
        thread->thread_ctx->thread_exit_state = TE_EXITED;
        unlock(&notifc->notifc_lock);
        return;
    }

    if (thread->thread_ctx->state != TS_WAITING) {
        unlock(&notifc->notifc_lock);
        return;
    }

#ifdef CKPT_IRQ_AND_NOTIFICATION_LAZY_COPY
    notification_lazy_copy_ckpt(notifc, true);
#endif

    thread_dq_cancel_node(thread->notif_dq_node_off);
    thread->notif_dq_node_off = QPTR_NULL;
    BUG_ON(notifc->waiting_threads_count <= 0);
    notifc->waiting_threads_count--;

    // int count = notifc->waiting_threads_count;
    // struct thread *thr;
    // for_each_in_list(thr, struct thread, notification_queue_node,
    // &notifc->waiting_threads) { 	count--;
    // }
    // BUG_ON(count);

    arch_set_thread_return(thread, -ETIMEDOUT);
    thread->thread_ctx->state = TS_TO_SCHED;
#ifdef DSM_ENABLED
    BUG_ON(rr_sched_enqueue_to_affinity(thread));
#else
    BUG_ON(sched_enqueue(thread));
#endif

    unlock(&notifc->notifc_lock);
}

/* Unlock futex lock iff eret_to_thread. */
int wait_notific_internal(struct notification *notifc, bool is_block,
                          struct timespec *timeout, bool need_unlock,
                          bool need_obj_put)
{
        int ret = 0;
        struct thread *thread;
        struct lock *futex_lock = NULL;

        lock(&notifc->notifc_lock);

        /* For recycling: the state is set in stop_notification */
        if (notifc->state == NOTIFIC_INVALID) {
                unlock(&notifc->notifc_lock);
                return -ECAPBILITY;
        }

        if (notifc->not_delivered_notifc_count > 0) {
                notifc->not_delivered_notifc_count--;
                ret = 0;
        } else {
                if (is_block) {
                        thread = current_thread;
                        /*
                         * queue_lock: grab the lock and then insert/remove
                         * a thread into one list.
                         */

                        lock(&thread->sleep_state.queue_lock);

                        /* Add this thread to waiting list */
                        thread_dq_enqueue(&notifc->waiting_threads, thread);
                        thread->thread_ctx->state = TS_WAITING;
                        notifc->waiting_threads_count++;
                        arch_set_thread_return(thread, 0);

                        if (timeout) {
                                thread->sleep_state.pending_notific = notifc;
                                enqueue_sleeper(
                                        thread, timeout, notific_timer_cb);
                        }

                        if (need_unlock) {
                                futex_lock = &(current_cap_group->futex->futex_lock);
                        }

                        /*
                         * Since current_thread is TS_WAITING,
                         * sched() will not put current_thread into the
                         * ready_queue.
                         *
                         * sched() must executed before unlock.
                         * Otherwise, current_thread maybe be notified and then
                         * its state will be set to TS_RUNNING. If so, sched()
                         * will put it into the ready_queue and it maybe
                         * directly switch to.
                         */
                        sched();

                        unlock(&thread->sleep_state.queue_lock);

                        unlock(&notifc->notifc_lock);

                        /* See the below impl of sys_notify */
                        if (need_obj_put) {
                                obj_put(notifc);
                        }

                        if (need_unlock) {
                                unlock(futex_lock);
                        }

                        eret_to_thread(switch_context());
                        /* The control flow will never reach here */

                } else {
                        ret = -EAGAIN;
                }
        }
        unlock(&notifc->notifc_lock);
        return ret;
}

/* Return 0 if wait successfully, -EAGAIN otherwise */
int wait_notific(struct notification *notifc, bool is_block,
                 struct timespec *timeout)
{
        return wait_notific_internal(notifc, is_block, timeout, false, true);
}

extern struct irq_notification *irq_notifcs[MAX_IRQ_NUM];

void wait_irq_notific(struct irq_notification *irq_notifc)
{
    struct notification *notifc;

    notifc = &(irq_notifc->notifc);
    lock(&notifc->notifc_lock);

    /* Add this thread to waiting list */
    thread_dq_enqueue(&notifc->waiting_threads, current_thread);
    current_thread->thread_ctx->state = TS_WAITING;
    notifc->waiting_threads_count++;
    arch_set_thread_return(current_thread, 0);

    // int count = notifc->waiting_threads_count;
    // struct thread *thr;
    // for_each_in_list(thr, struct thread, notification_queue_node,
    // &notifc->waiting_threads) { 	count--;
    // }
    // BUG_ON(count);

    irq_notifc->user_handler_ready = 1;

    sched();

    unlock(&notifc->notifc_lock);

    eret_to_thread(switch_context());
    /* The control flow will never reach here */
}

void signal_irq_notific(struct irq_notification *irq_notifc)
{
    struct notification *notifc;
    struct thread *target = NULL;

    notifc = &(irq_notifc->notifc);

    lock(&notifc->notifc_lock);

    irq_notifc->user_handler_ready = 0;

    /*
     * Some threads have been blocked and waiting for notifc.
     * Dequeue one waiting thread
     */
    target = thread_dq_dequeue(&notifc->waiting_threads);
    if (target != NULL) {
        target->notif_dq_node_off = QPTR_NULL;
        notifc->waiting_threads_count--;
    }

    // int count = notifc->waiting_threads_count;
    // 	count--;
    // }
    // BUG_ON(count);

    BUG_ON(target->thread_ctx->sc == NULL);

    // the interrupt direct to.

    BUG_ON(target->thread_ctx->affinity != NO_AFF
           && target->thread_ctx->affinity != smp_get_cpu_id());

    unlock(&notifc->notifc_lock);
    obj_put(irq_notifc);

    /*
     * In case the target thread is still executing wait_notific.
     */
    // TODO(FN): current chcore delete this line
    // while (target->thread_ctx->kernel_stack_state != KS_FREE) ;

    sched_to_thread(target);
}

void try_remove_timeout(struct thread *target)
{
    if (target == NULL)
        return;
    if (target->sleep_state.cb == NULL)
        return;

    try_dequeue_sleeper(target);

    target->sleep_state.pending_notific = NULL;
}

/**
 * signal_notific -- wake up one waiting thread
 * enqueue target thread to ready_queue
 */
int signal_notific(struct notification *notifc)
{
    struct thread *target = NULL;

    lock(&notifc->notifc_lock);

    /* For recycling: the state is set in stop_notification */
    if (notifc->state == NOTIFIC_INVALID) {
        unlock(&notifc->notifc_lock);
        return -ECAPBILITY;
    }

#ifdef CKPT_IRQ_AND_NOTIFICATION_LAZY_COPY
    notification_lazy_copy_ckpt(notifc, true);
#endif

    if (notifc->not_delivered_notifc_count > 0
        || notifc->waiting_threads_count == 0) {
        notifc->not_delivered_notifc_count++;
    } else {
        /*
         * Some threads have been blocked and waiting for notifc.
         * Dequeue one waiting thread
         */
        target = thread_dq_dequeue(&notifc->waiting_threads);

        if (target == NULL) {
            /* Queue was empty despite count > 0, possibly due to cancellations in progress */
            unlock(&notifc->notifc_lock);
            return -EAGAIN;
        }

        notifc->waiting_threads_count--;

#ifdef DSM_ENABLED
        // debug
        // if (CUR_MACHINE_ID == 1 && strstr(target->cap_group->cap_group_name, "pca")) {
        //     printk("signal_notific: target thread %p\n", target);
        //     print_thread(target);
        // }
        if (obj2object(target)->status == DSM_STATUS_MIGRATED) {
            struct object *object = dsm_get_inuse_object(obj2object(target), false);
            BUG_ON(!object);
            target = (struct thread *)object2obj(object);
        }
#endif

        if (target->thread_ctx == NULL) {
            printk("signal_notific: target thread %p\n", target);
            // print_thread(target);
        }

        BUG_ON(target == NULL || target->thread_ctx == NULL);

        /*
         * signal_notific may return -EAGAIN because of unable to lock.
         * The user-level library will transparently notify again.
         *
         * This is for preventing dead lock because handler_timer_irq
         * may already grab the queue_lock of a thread or the sleep_list lock.
         */
        if (try_lock(&target->sleep_state.queue_lock) != 0) {
            /* Lock failed: must be timeout now */
            unlock(&notifc->notifc_lock);
            return -EAGAIN;
        }

        /* TODO: cb != NULL indicates the thread is also in the sleep list */
        if (target->sleep_state.cb != NULL) {
            if (try_dequeue_sleeper(target) == false) {
                /* Failed to remove target in sleep list */
                unlock(&target->sleep_state.queue_lock);
                unlock(&notifc->notifc_lock);
                return -EAGAIN;
            }
        }

        /* Clear the dq node offset since thread was dequeued */
        target->notif_dq_node_off = QPTR_NULL;

        target->thread_ctx->state = TS_TO_SCHED;
        if (target->thread_ctx->thread_exit_state == TE_EXITING) {
            target->thread_ctx->thread_exit_state = TE_EXITED;
        } else if (target->thread_ctx->thread_exit_state == TE_STOPPING) {
            target->thread_ctx->thread_exit_state = TE_STOPPED;
        } else {
#ifdef DSM_ENABLED
            /* If target's affinity is on another machine, enqueue to its
             * shared queue so it is woken on the correct machine. */
            BUG_ON(rr_sched_enqueue_to_affinity(target));
#else
            BUG_ON(sched_enqueue(target));
#endif
        }

        unlock(&target->sleep_state.queue_lock);
    }

    unlock(&notifc->notifc_lock);

    return 0;
}

/* For FUTEX_REQUEUE only */
int requeue_notific(struct notification *src_notifc, struct notification *dst_notifc)
{
        struct thread *target = NULL;
        struct lock *big_lock = &dst_notifc->notifc_lock, *small_lock = &src_notifc->notifc_lock;
        int ret = 0;

        if (src_notifc == dst_notifc)
                return 0;

        if (src_notifc > dst_notifc) {
                big_lock = &src_notifc->notifc_lock;
                small_lock = &dst_notifc->notifc_lock;
        }
        lock(small_lock);
        lock(big_lock);

        /* For recycling: the state is set in stop_notification */
        if (src_notifc->state == NOTIFIC_INVALID
            || dst_notifc->state == NOTIFIC_INVALID) {
                ret = -ECAPBILITY;
                goto out_unlock;
        }
        if(src_notifc->waiting_threads_count == 0) {
                ret = 0;
                goto out_unlock;
        }

        target = thread_dq_dequeue(&src_notifc->waiting_threads);

        if (target == NULL) {
                /* Queue empty despite count > 0, possibly due to cancellations */
                ret = -EAGAIN;
                goto out_unlock;
        }

        src_notifc->waiting_threads_count--;

        if (try_lock(&target->sleep_state.queue_lock) != 0) {
                /* Lock failed: must be timeout now. Requeue back to src.
                 * Note: we already dequeued, so we need to enqueue back. */
                thread_dq_enqueue(&src_notifc->waiting_threads, target);
                src_notifc->waiting_threads_count++;
                ret = -EAGAIN;
                goto out_unlock;
        }

        /* Add this thread to dst waiting list */
        if (dst_notifc->not_delivered_notifc_count > 0) {
                BUG("Futex code should guarantee ZERO not_delivered_notifc_count.");
        } else {
                target->sleep_state.pending_notific = dst_notifc;
                thread_dq_enqueue(&dst_notifc->waiting_threads, target);
                dst_notifc->waiting_threads_count++;
        }

        unlock(&target->sleep_state.queue_lock);

out_unlock:
        unlock(big_lock);
        unlock(small_lock);

        return ret;
}

int sys_create_notifc(void)
{
    struct notification *notifc = NULL;
    int notifc_cap = 0;
    int ret = 0;

    notifc = obj_alloc(TYPE_NOTIFICATION, sizeof(*notifc), __MT_SHARED__);
    if (!notifc) {
        ret = -ENOMEM;
        goto out_fail;
    }
    init_notific(notifc);

    notifc_cap = cap_alloc(current_cap_group, notifc, 0);
    if (notifc_cap < 0) {
        ret = notifc_cap;
        goto out_free_obj;
    }

    return notifc_cap;
out_free_obj:
    obj_free(notifc);
out_fail:
    return ret;
}

int sys_wait(u32 notifc_cap, bool is_block, struct timespec *timeout)
{
    struct notification *notifc = NULL;
    struct timespec timeout_k;
    int ret;

    notifc = obj_get(current_thread->cap_group, notifc_cap, TYPE_NOTIFICATION);
    if (!notifc) {
        ret = -ECAPBILITY;
        goto out;
    }

    if (timeout) {
        ret = copy_from_user(
                (char *)&timeout_k, (char *)timeout, sizeof(timeout_k));
        if (ret != 0)
            goto out_obj_put;
    }

    ret = wait_notific(notifc, is_block, timeout ? &timeout_k : NULL);
out_obj_put:
    obj_put(notifc);
out:
    return ret;
}

int sys_notify(u32 notifc_cap)
{
    struct notification *notifc = NULL;
    int ret;
    notifc = obj_get(current_thread->cap_group, notifc_cap, TYPE_NOTIFICATION);
    if (!notifc) {
        ret = -ECAPBILITY;
        goto out;
    }
    ret = signal_notific(notifc);
    obj_put(notifc);
out:
    return ret;
}
