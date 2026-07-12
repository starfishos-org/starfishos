#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"

#ifdef REPORT
extern u64 eval_obj_time[];
#endif

int notification_ckpt(struct notification *notifc,
                      struct ckpt_notification *ckpt_notifc, int flags)
{
    /* NOTE: With durable_queue, we don't save waiting threads during checkpoint.
     * Threads in the waiting queue are in a temporary waiting state and can be
     * restarted from scratch after restore. This is a reasonable trade-off since
     * checkpoint typically saves long-running stable state, not transient waits.
     */
    ckpt_notifc->not_delivered_notifc_count =
            notifc->not_delivered_notifc_count;
    ckpt_notifc->waiting_threads_count = 0;  /* Don't save waiting threads */
    if (ckpt_notifc->waiting_thread_roots)
        kfree(ckpt_notifc->waiting_thread_roots);
    ckpt_notifc->waiting_thread_roots = NULL;
    ckpt_notifc->state = notifc->state;

    return 0;
}

int notification_restore(struct object *notifc_obj,
                         struct ckpt_object *ckpt_notifc_obj,
                         struct kvs *obj_map, int flags)
{
    int r;
    struct notification *notifc = (struct notification *)notifc_obj->opaque;
    struct ckpt_notification *ckpt_notifc =
            (struct ckpt_notification *)ckpt_notifc_obj->opaque;

    CFORK_LOG_DEBUG("%s: notifc_obj: %p, ckpt_notifc_obj: %p\n",
        __func__, notifc_obj, ckpt_notifc_obj);

    lock_init(&notifc->notifc_lock);
    r = thread_dq_init(&notifc->waiting_threads);
    if (r != 0) {
        goto out_fail;
    }
    notifc->not_delivered_notifc_count =
            ckpt_notifc->not_delivered_notifc_count;
    notifc->waiting_threads_count = 0;  /* We don't restore waiting threads */
    notifc->state = ckpt_notifc->state;

    return 0;
out_fail:
    return r;
}

int ckpt_notification_copy(struct ckpt_object *src_obj,
                           struct ckpt_object *dst_obj, struct kvs *obj_map)
{
    struct ckpt_notification *src, *dst;
    int i;

    src = (struct ckpt_notification *)src_obj->opaque;
    dst = (struct ckpt_notification *)dst_obj->opaque;

    /* Copy basic fields */
    dst->not_delivered_notifc_count = src->not_delivered_notifc_count;
    dst->waiting_threads_count = src->waiting_threads_count;
    dst->state = src->state;

    /* Allocate memory for waiting_thread_roots */
    dst->waiting_thread_roots =
            kmalloc(src->waiting_threads_count * sizeof(struct ckpt_obj_root *),
                    __MT_SHARED__);
    if (!dst->waiting_thread_roots) {
        return -ENOMEM;
    }

    /* Copy waiting thread roots */
    for (i = 0; i < src->waiting_threads_count; i++) {
        dst->waiting_thread_roots[i] =
                get_copied_obj_root(src->waiting_thread_roots[i], obj_map);
        if (!dst->waiting_thread_roots[i]) {
            kfree(dst->waiting_thread_roots);
            return -ENOMEM;
        }
    }

    return 0;
}
