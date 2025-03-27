#include <object/object.h>
#include <ipc/notification.h>
#include <object/thread.h>
#include <dsm/tiering.h>

#include "../dsm_tiering.h"

static inline void notification_copy(struct notification *src, struct notification *dst, mem_t mem_type)
{
    dst->not_delivered_notifc_count = src->not_delivered_notifc_count;
    dst->waiting_threads_count = src->waiting_threads_count;
    dst->state = src->state;
    init_list_head(&dst->waiting_threads);

    struct thread *old_thread;
    for_each_in_list (old_thread, struct thread, notification_queue_node,
                      &src->waiting_threads) {
        /* remove from old queue and enqueue new queue */
        // TODO: concurrent issue?
        list_del(&old_thread->notification_queue_node);
        list_append(&old_thread->notification_queue_node,
                    &dst->waiting_threads);
    }
}

/**
 * demote a private_notifc to shared_notifc on shared memory
 * 
 * @param private_notifc: the private notification to be demoted
 * @param shared_notifc: the shared notification to be copied to
 * @return 0 on success, -EINVAL on failure
 */
int dsm_copy_notification(struct object *src_obj, struct object *dst_obj)
{
    struct notification *src_notifc = (struct notification *)src_obj->opaque;
    struct notification *dst_notifc = (struct notification *)dst_obj->opaque;
    int is_demote = is_private_object(src_obj);
    
    /* Stop the notification first */
    extern void stop_notification(struct notification *notifc); // recycle.c
    stop_notification(src_notifc);

    /* Copy basic fields */
    notification_copy(src_notifc, dst_notifc, is_demote ? __MT_SHARED__ : __MT_PRIVATE__);

    /* Demote waiting threads */
    if (dst_notifc->waiting_threads_count > 0) {
        struct thread *thread;
        struct object *thread_obj;
        for_each_in_list (thread, struct thread, notification_queue_node,
                          &dst_notifc->waiting_threads) {
            thread_obj = container_of(thread, struct object, opaque);;
            /* a shared notification can not notify a private thread */
            if (is_demote && is_private_object(thread_obj)) {
                dsm_demote_object(thread_obj);
            }
        }
    }

    return 0;
}
