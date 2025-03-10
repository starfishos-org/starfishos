#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"

#ifdef REPORT
extern u64 eval_obj_time[];
#endif

int irq_ckpt(struct irq_notification *irq_notifc,
             struct ckpt_irq_notification *ckpt_irq_notifc, int flags)
{
    ckpt_irq_notifc->intr_vector = irq_notifc->intr_vector;
    ckpt_irq_notifc->status = irq_notifc->status;
    ckpt_irq_notifc->user_handler_ready = irq_notifc->user_handler_ready;
    return notification_ckpt(
            &irq_notifc->notifc, &ckpt_irq_notifc->notifc, flags);
}

int irq_restore(struct object *irq_obj, struct ckpt_object *ckpt_irq_obj,
                struct kvs *obj_map, int flags)
{
    int i, r;
    struct irq_notification *irq_notifc =
            (struct irq_notification *)irq_obj->opaque;
    struct ckpt_irq_notification *ckpt_irq_notifc =
            (struct ckpt_irq_notification *)ckpt_irq_obj->opaque;
    struct notification *notifc = &irq_notifc->notifc;
    struct ckpt_notification *ckpt_notifc = &ckpt_irq_notifc->notifc;
    struct thread *new_thread;
    struct object *new_obj;
    struct ckpt_obj_root *ckpt_obj_root;

    irq_notifc->intr_vector = ckpt_irq_notifc->intr_vector;
    irq_notifc->status = ckpt_irq_notifc->status;
    irq_notifc->user_handler_ready = ckpt_irq_notifc->user_handler_ready;

    lock_init(&notifc->notifc_lock);
    init_list_head(&notifc->waiting_threads);
    notifc->not_delivered_notifc_count =
            ckpt_notifc->not_delivered_notifc_count;
    notifc->waiting_threads_count = ckpt_notifc->waiting_threads_count;
    for (i = 0; i < notifc->waiting_threads_count; i++) {
        ckpt_obj_root = ckpt_notifc->waiting_thread_roots[i];
        new_obj = restore_obj_get(ckpt_obj_root);
        if (!new_obj) {
            r = -ENOMEM;
            BUG_ON(1);
            goto out_fail;
        }
        new_thread = (struct thread *)new_obj->opaque;
        list_append(&new_thread->notification_queue_node,
                    &notifc->waiting_threads);
        new_thread->sleep_state.pending_notific = notifc;
    }
    notifc->state = ckpt_notifc->state;

    return 0;
out_fail:
    return r;
}

int ckpt_irq_copy(struct ckpt_object *src_obj, struct ckpt_object *dst_obj,
                  struct kvs *obj_map)
{
    struct ckpt_irq_notification *src_irq, *dst_irq;
    int r;

    src_irq = (struct ckpt_irq_notification *)src_obj->opaque;
    dst_irq = (struct ckpt_irq_notification *)dst_obj->opaque;

    /* Copy basic fields */
    dst_irq->intr_vector = src_irq->intr_vector;
    dst_irq->status = src_irq->status;
    dst_irq->user_handler_ready = src_irq->user_handler_ready;

    /* Copy the notification part */
    r = ckpt_notification_copy((struct ckpt_object *)&src_irq->notifc,
                               (struct ckpt_object *)&dst_irq->notifc,
                               obj_map);
    if (r) {
        return r;
    }

    return 0;
}
