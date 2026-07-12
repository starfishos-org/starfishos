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
    int r;

    /* Copy basic fields */
    dst_notifc->not_delivered_notifc_count = src_notifc->not_delivered_notifc_count;
    dst_notifc->waiting_threads_count = 0;  /* Don't copy waiting threads */
    dst_notifc->state = src_notifc->state;

    /* Init lock */
    lock_init(&dst_notifc->notifc_lock);

    /* Initialize waiting threads queue */
    r = thread_dq_init(&dst_notifc->waiting_threads);
    if (r != 0) {
        return r;
    }

    return 0;
}

int dsm_stw_copy_notification(struct object *src_obj, struct object *dst_obj)
{
    return 0;
}