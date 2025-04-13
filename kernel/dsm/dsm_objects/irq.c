#include <object/irq.h>
#include <dsm/tiering.h>

#include "../dsm_tiering.h"

int dsm_copy_irq(struct object *src_obj, struct object *dst_obj)
{
    struct irq_notification *src_irq = (struct irq_notification *)src_obj->opaque;
    struct irq_notification *dst_irq = (struct irq_notification *)dst_obj->opaque;
    int is_demote = is_private_object(src_obj);

    /* Copy basic fields */
    dst_irq->intr_vector = src_irq->intr_vector;
    dst_irq->status = src_irq->status;
    dst_irq->user_handler_ready = src_irq->user_handler_ready;

    /* Copy notification part */
    dst_irq->notifc.not_delivered_notifc_count = src_irq->notifc.not_delivered_notifc_count;
    dst_irq->notifc.waiting_threads_count = src_irq->notifc.waiting_threads_count;
    dst_irq->notifc.state = src_irq->notifc.state;

    /* Initialize waiting threads list */
    init_list_head(&dst_irq->notifc.waiting_threads);

    /* Threads will be handled separately */
    struct thread *thread;
    struct object *thread_obj;
    for_each_in_list (thread, struct thread, notification_queue_node,
                        &dst_irq->notifc.waiting_threads) {
        thread_obj = container_of(thread, struct object, opaque);;
        /* a shared notification can not notify a private thread */
        if (is_demote && is_private_object(thread_obj)) {
            dsm_demote_object(thread_obj);
        }
    }

    return 0;
}

int dsm_ckpt_irq(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}