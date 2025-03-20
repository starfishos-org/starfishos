#include <common/kvstore.h>
#include <common/util.h>
#include <common/waitlist.h>
#include <mm/kmalloc.h>
#include <object/cap_group.h>
#include <object/thread.h>
#include <irq/ipi.h>
#include <irq/timer.h>
#include <sched/sched.h>
#include <ckpt/ckpt.h>

#include "cfork.h"
#include "ckpt_object_pool.h"
#include "ckpt_ws.h"

/**
 * cfork_start_threads: start all threads in the list.
 * @param thread_list: the list of threads to be started
 * @return 0 if all threads are started, -EINVAL otherwise.
 */
int cfork_start_threads(struct list_head *thread_list)
{
    struct thread *thread, *thread_tmp;

    // enqueue all threads to the scheduler
    for_each_in_list_safe(thread, thread_tmp, node, thread_list) {
        BUG_ON((thread->thread_ctx->type != TYPE_USER));
        // currently, clear affinity to -1
        thread->thread_ctx->cpuid = NO_AFF;
        CFORK_LOG_DEBUG("thread: %p, state: %d enqueued\n", thread, thread->thread_ctx->state);
        BUG_ON(sched_enqueue(thread));
    }

    return 0;
}

/**
 * Stop all threads in the list.
 * @param thread_list: the list of threads to be stopped
 * @return 0 if all threads are stopped, -EINVAL otherwise.
 */
int cfork_stop_threads(struct list_head *thread_list)
{
    struct thread *thread, *thread_tmp;
    wait_node_t *node, *node_tmp;
    struct list_head waiting_thread_list; // local list

    init_list_head(&waiting_thread_list);

    for_each_in_list_safe(thread, thread_tmp, node, thread_list) {

        if (thread->thread_ctx->type != TYPE_USER) {
            // only user threads are allowed to be stopped
            CFORK_LOG_ERR("Only user threads are allowed to be stopped");
            return -EINVAL;
        }

        switch (thread->thread_ctx->state) {
        case TS_RUNNING:
            // send signal to the running thread
            thread->thread_ctx->state = TS_STOPPING;
            send_ipi(thread->thread_ctx->cpuid, IPI_RESCHED);
            add_to_waiting_list(&waiting_thread_list, (void *)thread);
            break;
        case TS_WAITING:
            // TODO: who want to wake up the thread?
            // notify: maintain cross-machine notify

            // timer: remove the thread from the sleep queue
            while (!try_dequeue_sleeper(thread));

            thread->thread_ctx->state = TS_STOPPED;
            break;
        case TS_INTER:
        case TS_INIT:
            // TODO: check if there are corner cases
        case TS_READY:
            // mark the thread as migrating
            thread->thread_ctx->state = TS_STOPPED;
            break;
        default:
            BUG("Unexpected thread state: %d", thread->thread_ctx->state);
            break;
        }
    }

    /* Loop until all threads are stopped */
    while (!list_empty(&waiting_thread_list)) {
        for_each_in_waitlist_safe(node, node_tmp, &waiting_thread_list) {
            thread = (struct thread *)node->data;
            if (thread->thread_ctx->state == TS_STOPPED) {
                remove_from_waiting_list(&waiting_thread_list, node);
            }
        }
    }

    return 0;
}

/**
 * cfork_prepare_ckpt_process: prepare the ckpt_objs for the cap group.
 * @param root_cg_obj: the root object of the cap group
 * @return the ckpt_obj_root of the cap group
 */
struct ckpt_obj_root *
cfork_prepare_ckpt_process(struct object *root_cg_obj)
{
    struct ckpt_obj_root *root_cg_obj_root;

    /* get the root object of the cap group */
    root_cg_obj_root = ckpt_obj_root_get(root_cg_obj, FLAGS_ALLOC | FLAGS_CFORK);

    /* prepare the ckpt_objs for the cap group */
    root_cg_obj_root->cfork_ckpt_obj = ckpt_obj_alloc(TYPE_CAP_GROUP);

    return root_cg_obj_root;
}


/**
 * cfork_ckpt_process: checkpoint the cap group.
 * @param root_cg_obj: the object contains the cap group to be checkpointed
 * @param root_obj_root: the ckpt_object contains the checkpointed cap group
 * @return 0 if the checkpoint is successful, -EINVAL otherwise.
 * 
 * Different from whole system checkpoint and whole process checkpoint,
 * cfork checkpoint only copy the cap group and its threads.
 * Since all other objects are expected to be checkpointed in the 
 * cfork_prepare_process() function.
 */
int cfork_ckpt_process(struct ckpt_obj_root *root_cg_obj_root)
{
    /* checkpoint the sub captree */
    struct ckpt_object *ckpt_obj;
    int ret = 0;

    BUG_ON(!root_cg_obj_root);
    
    /* checkpoint the cap group */
    ckpt_obj = ckpt_obj_get(root_cg_obj_root, FLAGS_ALLOC | FLAGS_CFORK);
    if (!ckpt_obj) {
        CFORK_LOG_ERR("Failed to checkpoint the cap group");
        return -EINVAL;
    }

    return ret;
}

/**
 * cfork_restore_process: restore the cap group.
 * @param ckpt_obj_root: the ckpt_object contains the checkpointed cap group
 * @return the restored cap group
 */
int cfork_restore_process(struct ckpt_obj_root *ckpt_obj_root, struct cap_group **out_root_cg)
{
    struct kvs *obj_map;
    struct object *obj;
    struct cap_group *cg;
    
    // contains tmp mapping from ckpt_obj_root to the restored object
    obj_map = new_kvs(KVS_SIZE, __PRIVATE__);
    if (!obj_map) {
        CFORK_LOG_ERR("Failed to allocate the obj_map");
        return -ENOMEM;
    }

    // append ckpt_obj_root to root_cg
    obj = restore_obj_get_by_cap_group(
            ckpt_obj_root, obj_map, FLAGS_ALLOC | FLAGS_CFORK);
    if (!obj) {
        CFORK_LOG_ERR("Failed to restore the object");
        kvs_destroy(obj_map);
        return -EINVAL;
    }

    CFORK_LOG_DEBUG("restored the object: %p", obj);

    // destroy the tmp obj_map
    // FIXME(FN): kvs_destroy? will cause a NULL pointer dereference
    kvs_free(obj_map);

    if (obj->type != TYPE_CAP_GROUP) {
        CFORK_LOG_ERR("Unexpected object type: %d", obj->type);
        return -EINVAL;
    }

    /* set the output */
    cg = (struct cap_group *)(obj->opaque);
    *out_root_cg = cg;

    return 0;
}

int add_cap_group_to_cap_tree(struct cap_group *root_cap_group, struct cap_group *restored_cg)
{
    return 0;
}
