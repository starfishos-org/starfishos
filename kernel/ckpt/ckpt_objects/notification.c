#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"

#ifdef REPORT
extern u64 eval_obj_time[];
#endif

int notification_ckpt(struct notification *notifc,
                      struct ckpt_notification *ckpt_notifc, int alloc)
{
    int r, count;
    struct thread *old_thread;
    struct object *old_obj;
    struct ckpt_obj_root *ckpt_obj_root;

    ckpt_notifc->not_delivered_notifc_count =
            notifc->not_delivered_notifc_count;
    ckpt_notifc->waiting_threads_count = notifc->waiting_threads_count;
    if (ckpt_notifc->waiting_thread_roots)
        kfree(ckpt_notifc->waiting_thread_roots);
    ckpt_notifc->waiting_thread_roots = kmalloc(
            ckpt_notifc->waiting_threads_count * sizeof(struct ckpt_obj_root *),
            __DEFAULT__);

    count = 0;
    for_each_in_list (old_thread,
                      struct thread,
                      notification_queue_node,
                      &notifc->waiting_threads) {
        old_obj = container_of(old_thread, struct object, opaque);
        ckpt_obj_root = ckpt_obj_root_get(old_obj, alloc);
        if (!ckpt_obj_root) {
            BUG_ON(1);
            r = -ENOMEM;
            goto out_fail;
        }
        ckpt_notifc->waiting_thread_roots[count] = ckpt_obj_root;
        count++;
    }
    BUG_ON(count != ckpt_notifc->waiting_threads_count);
    ckpt_notifc->state = notifc->state;

    return 0;
out_fail:
    return r;
}

int notification_restore(struct object *notifc_obj,
                         struct ckpt_object *ckpt_notifc_obj,
                         struct kvs *obj_map)
{
    int r, i;
    struct notification *notifc = (struct notification *)notifc_obj->opaque;
    struct ckpt_notification *ckpt_notifc =
            (struct ckpt_notification *)ckpt_notifc_obj->opaque;
    struct thread *new_thread;
    struct object *new_obj;
    struct ckpt_obj_root *ckpt_obj_root;

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

    // int count = notifc->waiting_threads_count;
    // struct thread *thr;
    // for_each_in_list(thr, struct thread, notification_queue_node,
    // &notifc->waiting_threads) { 	count--;
    // }
    // BUG_ON(count);

    return 0;
out_fail:
    return r;
}