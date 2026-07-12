#include <object/irq.h>
#include <dsm/tiering.h>

#include "../dsm_tiering.h"

int dsm_copy_irq(struct object *src_obj, struct object *dst_obj)
{
    struct irq_notification *src_irq = (struct irq_notification *)src_obj->opaque;
    struct irq_notification *dst_irq = (struct irq_notification *)dst_obj->opaque;
    int r;

    /* Copy basic fields */
    dst_irq->intr_vector = src_irq->intr_vector;
    dst_irq->status = src_irq->status;
    dst_irq->user_handler_ready = src_irq->user_handler_ready;

    /* Copy notification part */
    dst_irq->notifc.not_delivered_notifc_count = src_irq->notifc.not_delivered_notifc_count;
    dst_irq->notifc.waiting_threads_count = 0;  /* Don't copy waiting threads */
    dst_irq->notifc.state = src_irq->notifc.state;

    /* Initialize waiting threads queue */
    r = thread_dq_init(&dst_irq->notifc.waiting_threads);
    if (r != 0) {
        return r;
    }

    return 0;
}

int dsm_stw_copy_irq(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}