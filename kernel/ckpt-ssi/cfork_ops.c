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
#include <dsm/tiering.h>

#include "cfork.h"
#include "ckpt_object_pool.h"
#include "ckpt_ws.h"

static int start_user_thread(struct thread *thread)
{
    int ret = 0;
    BUG_ON(thread->thread_ctx->type != TYPE_USER);

    thread->thread_ctx->thread_exit_state = TE_RUNNING;

    // TODO: handle thread with affinity or fpu_owner
    if (thread->thread_ctx->affinity != NO_AFF 
            || thread->thread_ctx->is_fpu_owner >= 0) {
        CFORK_LOG_DEBUG("thread with affinity or fpu_owner: %p, affinity: %d, is_fpu_owner: %d\n", 
            thread, thread->thread_ctx->affinity, thread->thread_ctx->is_fpu_owner);
        // return -EINVAL;
    }
    thread->thread_ctx->cpuid = NO_AFF;
    thread->thread_ctx->is_fpu_owner = -1;

    switch (thread->thread_ctx->state) {
    case TS_RUNNING:
    case TS_READY:
        /* mark thread as inter state to pass check in __sched_enqueue */
        thread->thread_ctx->state = TS_INTER;
        ret = sched_enqueue(thread);
        if (ret < 0) {
            CFORK_LOG_ERR("failed to enqueue thread: %p\n", thread);
            print_thread(thread);
            return ret;
        }
        break;
    case TS_WAITING:
        break;
    default:
        CFORK_LOG_ERR("illegal thread: %p with state: %s\n", 
            thread, thread_state[thread->thread_ctx->state]);
        return -EINVAL;
    }

    return ret;
}

/**
 * start_all_threads: start all threads in the list.
 * @param thread_list: the list of threads to be started
 * @return 0 if all threads are started, -EINVAL otherwise.
 */
int start_all_threads(struct list_head *thread_list)
{
    struct thread *thread, *thread_tmp;
    // int ret;

    CFORK_LOG_INFO("start_all_threads:\n");

    // enqueue all threads to the scheduler
    for_each_in_list_safe (thread, thread_tmp, node, thread_list) {
        print_thread(thread);
        kprint_vmr(thread->vmspace);

        BUG_ON(thread->thread_ctx->thread_exit_state != TE_STOPPED);

        /* promote the thread to local memory */
        // ret = dsm_promote_object(obj2object(thread));
        // if (ret) {
        //     CFORK_LOG_WARN("failed to promote thread: %p\n", thread);
        // }

        /* start the thread */
        switch (thread->thread_ctx->type) {
            case TYPE_USER:
                start_user_thread(thread);
                break;
            case TYPE_SHADOW:
            case TYPE_REGISTER:
                // TODO: add shadow and register thread start logic here
            default:
                CFORK_LOG_ERR("%d: unsupported thread type\n", __LINE__);
                return -EINVAL;
        }
    }

    CFORK_LOG_DEBUG("all threads have been started\n");

    return 0;
}

/**
 * stop_all_threads: stop all threads in the list.
 * @param thread_list: the list of threads to be stopped
 * @return 0 if all threads are stopped, -EINVAL otherwise.
 */
int stop_all_threads(struct list_head *thread_list)
{
    struct thread *thread, *thread_tmp;
    wait_node_t *node, *node_tmp;
    struct list_head waiting_thread_list; // local list
    int ret = 0;

    init_list_head(&waiting_thread_list);

    for_each_in_list_safe(thread, thread_tmp, node, thread_list) {
        print_thread(thread);
        thread->thread_ctx->thread_exit_state = TE_STOPPING;

        switch (thread->thread_ctx->state) {
        case TS_RUNNING:
            /* ask the cpu to reschedule a common thread that is running */
            send_ipi(thread->thread_ctx->cpuid, IPI_RESCHED);
            add_to_waiting_list(&waiting_thread_list, (void *)thread);
            break;
        case TS_READY:
            ret = sched_dequeue(thread);
            /** case1:
             * @here: thread is TS_READY
             * @scheduler: thread = find_runnable_thread(); 
             *          which dequeue thread and set it to TS_INTER
             * @here: rr_sched_dequeue will fail because state is not TS_READY
             * this thread might be running; should wait
             */
            if (ret < 0) {
                add_to_waiting_list(&waiting_thread_list, (void *)thread);
            } 
            /** case 2:
             * @here: thread is TS_READY, dequeue success
             * @scheduler: fail to dequeue the thread
             * this thread is really stopped
             */
            else {
                thread->thread_ctx->thread_exit_state = TE_STOPPED;
            }
            break;
        case TS_WAITING_IPC:
            /* If waiting for ipc finish, ipc_return will set thread as TE_EXITED */
            add_to_waiting_list(&waiting_thread_list, (void *)thread);
            break;
        case TS_WAITING:
            /* If waiting for timer, remove it from sleeping queue */
            if (thread->sleep_state.cb != NULL) {
                // kinfo("try to remove timeout for thread: %p\n", thread);
                extern void try_remove_timeout(struct thread *);
                try_remove_timeout(thread);
                thread->thread_ctx->state = TS_READY;
            }
            /* directly checkpoint */
            thread->thread_ctx->thread_exit_state = TE_STOPPED;
            break;
        case TS_INTER:
        case TS_INIT:
            /* mark the thread as TE_STOPPED */
            thread->thread_ctx->thread_exit_state = TE_STOPPED;
            break;
        default:
            BUG("unsupported thread state: %s\n", thread_state[thread->thread_ctx->state]);
        }
    }

    /* Loop until all threads are stopped */
    while (!list_empty(&waiting_thread_list)) {
        for_each_in_waitlist_safe(node, node_tmp, &waiting_thread_list) {
            thread = (struct thread *)node->data;
            if (thread->thread_ctx->thread_exit_state == TE_STOPPED) {
                // check thread is not holding the kernel stack
                BUG_ON(thread->thread_ctx->kernel_stack_state != KS_FREE);
                BUG_ON(thread->thread_ctx->state == TS_EXIT);
                remove_from_waiting_list(&waiting_thread_list, node);
            } else {
                // print_thread(thread);
                /* If the thread is scheduled to running */
                if (thread->thread_ctx->state == TS_RUNNING) {
                    send_ipi(thread->thread_ctx->cpuid, IPI_RESCHED);
                }
            }
        }

        /* Handle IPI tx while waiting to avoid deadlock. */
        handle_ipi();
    }

    CFORK_LOG_DEBUG("%s: all threads have been stoped:\n", __func__);
    for_each_in_list_safe(thread, thread_tmp, node, thread_list) {
        print_thread(thread);
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

    /* demote everything except cap group */
    // dsm_migrate_process_prepare(root_cg_obj)

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
    obj_map = new_kvs(KVS_SIZE, __MT_PRIVATE__);
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
