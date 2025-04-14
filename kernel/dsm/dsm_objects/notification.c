#include <object/object.h>
#include <ipc/notification.h>
#include <object/thread.h>
#include <dsm/tiering.h>
#include <common/lock.h>
#include "../dsm_tiering.h"

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
    mem_t mem_type = is_demote ? __MT_SHARED__ : __MT_PRIVATE__;

    /* Copy basic fields */
    dst_notifc->not_delivered_notifc_count = src_notifc->not_delivered_notifc_count;
    dst_notifc->waiting_threads_count = src_notifc->waiting_threads_count;
    dst_notifc->state = src_notifc->state;

    /* Init lock */
    lock_init(&dst_notifc->notifc_lock);

    /* Initialize waiting threads list */
    init_list_head(&dst_notifc->waiting_threads);
    if (dst_notifc->waiting_threads_count > 0) {
        struct thread *thread, *new_thread, *tmp;
        struct object *thread_object, *new_thread_object;
        for_each_in_list_safe (thread, tmp, notification_queue_node,
                          &dst_notifc->waiting_threads) {
            thread_object = obj2object(thread);
            /* get shared thread object */
            new_thread_object = dsm_get_object_by_mem_type(thread_object, mem_type, true);
            /* set type to bridge thread */
            new_thread_object->type = DSM_TYPE_THREAD_NOTIFY_BRIDGE;
            BUG_ON(!new_thread_object);

            new_thread =(struct thread *)object2obj(new_thread_object);
            /* set machine id to current machine id */
            // NOTE: the following will be copied when demote thread
            new_thread->machine_id = thread->machine_id;
            if (thread->thread_ctx->cpuid == NO_AFF) {
                new_thread->thread_ctx->cpuid = NO_AFF;
            } else {
                new_thread->thread_ctx->cpuid = thread->thread_ctx->cpuid;
            }
            /* NOTE: old thread should not be recycled!!! */

            list_del(&thread->notification_queue_node);
            list_append(&new_thread->notification_queue_node,
                        &dst_notifc->waiting_threads);
            
            printk("copy notification: new thread %p\n", new_thread);
            print_thread(new_thread);
        }
    }

    return 0;
}

int dsm_stw_copy_notification(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}