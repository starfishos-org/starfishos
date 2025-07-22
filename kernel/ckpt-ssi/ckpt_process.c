#include <common/kvstore.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <mm/mm.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <object/cap_group.h>
#include <object/thread.h>
#include <object/user_fault.h>
#include <ckpt/ckpt.h>
#include <ckpt/ckpt_data.h>
#include <sched/context.h>
#include <dsm/dsm-single.h>
#include <dsm/tiering.h>

#include "cfork.h"
#include "ckpt_log.h"
#include "ckpt_object_pool.h"
#include "ckpt_objects.h"

/**
 * ckpt_process: checkpoint the cap group.
 * @param root_cg_obj: the object contains the cap group to be checkpointed
 * @param root_obj_root: the ckpt_object contains the checkpointed cap group
 * @return 0 if the checkpoint is successful, -EINVAL otherwise.
 * 
 * Different from whole system checkpoint and whole process checkpoint,
 * cfork checkpoint only copy the cap group and its threads.
 * Since all other objects are expected to be checkpointed in the 
 * cfork_prepare_process() function.
 */
int ckpt_process(struct ckpt_obj_root *root_cg_obj_root)
{
    /* checkpoint the sub captree */
    struct ckpt_object *ckpt_obj;
    int ret = 0;

    BUG_ON(!root_cg_obj_root);

    /* checkpoint the cap group */
    ckpt_obj = ckpt_obj_get(root_cg_obj_root, FLAGS_ALLOC);
    if (!ckpt_obj) {
        CKPT_LOG_ERR("Failed to checkpoint the cap group");
        return -EINVAL;
    }

    return ret;
}

extern char *pname_to_pname_ptr(u64 pname_ptr, u64 pname_len);

// user program needs to call frequently sys_ckpt_process   
int sys_ckpt_process(u64 pname_ptr, u64 pname_len) 
{
    char *pname;
    int ret = 0;
    struct cap_group *cap_group;
    // struct object *cg_obj;
    struct ckpt_obj_root *ckpt_obj_root;
    // bool first_time = false;

    pname = pname_to_pname_ptr(pname_ptr, pname_len);

    ckpt_obj_root = find_ckpt_obj_root_by_name(pname, pname_len);

    // The first time of checkpoint
    if (unlikely(ckpt_obj_root)) {
        // first_time = true;
        // find the cap group in the cap tree by the process name
        cap_group = find_capgroup_by_name(pname, pname_len);
        if (unlikely(!cap_group)) {
            CKPT_LOG_ERR("%s: cap_group not found\n", __func__);
            ret = -ENOENT;
            goto out;
        }

        // checkpoint most memory expect cap_group and thread to the shared memory
        ckpt_obj_root = ckpt_obj_root_get(obj2object(cap_group), FLAGS_ALLOC);

        /* prepare the ckpt_objs for the cap group */
        // ckpt_obj_root->ckpt_objs[0] = ckpt_obj_alloc(TYPE_CAP_GROUP);
        // if (!cg_obj_root) {
        //     CKPT_LOG_ERR("cfork_prepare: cfork_prepare_ckpt_process failed\n");
        //     goto out;
        // }

        // add the cg_obj_root to the kvs
        ret = add_ckpt_obj_root_by_name(ckpt_obj_root, pname, pname_len);
        if (ret) {
            CKPT_LOG_ERR("%s: add_ckpt_obj_root_by_name failed\n", __func__);
            goto out;
        }

        CFORK_LOG_INFO("%s: prepare %s done\n", pname);
    } else {
        cap_group = (struct cap_group *)object2obj(ckpt_obj_root->obj);
    }

    BUG_ON(!ckpt_obj_root);
    BUG_ON(!cap_group);

    /* flip the flip-flag */
    system_current_flip_flag ^= 1;

    ret = stop_all_threads(&cap_group->thread_list);
    if (unlikely(ret)) {
        CKPT_LOG_ERR("%s: stop_all_threads failed\n", __func__);
        goto out;
    }

    // start the checkpoint
    ret = ckpt_process(ckpt_obj_root);
    if (unlikely(ret)) {
        CKPT_LOG_ERR("%s: ckpt_process failed\n", __func__);
        goto out;
    }

    ret = start_all_threads(&cap_group->thread_list);
    if (unlikely(ret)) {
        CKPT_LOG_ERR("%s: start_all_threads failed\n", __func__);
        goto out;
    }

out:
    kfree(pname);
    return ret;
}

int sys_restore_process(u64 pname_ptr, u64 pname_len)
{
    return 0;
}
