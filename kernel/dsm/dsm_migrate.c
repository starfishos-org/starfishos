#include <dsm/tiering.h>
#include <object/cap_group.h>
#include <common/list.h>
#include <object/thread.h>
#include <mm/vmspace.h>
#include <object/object.h>
#include <dsm/dsm-single.h>

#include "dsm_tiering.h"

static int reinstall_system_services(struct cap_group *cap_group)
{
    /* Lazy reinstall system services */
    return 0;
}

int dsm_migrate_process_prepare(struct object *root_cg_obj)  
{
    int flags = 0;
    int ret = 0;
    
    /* mask out cap group and thread */
    flags = ~((1L << TYPE_CAP_GROUP) | (1L << TYPE_THREAD));

    /* demote all objects except cap group and thread */
    ret = demote_each_object_in_cap_group((struct cap_group *)object2obj(root_cg_obj), flags);
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to demote objects in cap group %p\n", __func__, root_cg_obj);
        return ret;
    }

    return 0;
}


int dsm_migrate_process_ckpt(struct object *src_cap_group_obj)
{
    int ret = 0;
    struct cap_group *src_cap_group, *dst_cap_group;

    src_cap_group = (struct cap_group *)object2obj(src_cap_group_obj);

    /* demote the threads */
    ret = demote_each_object_in_cap_group(src_cap_group, 1L << TYPE_THREAD);
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to demote thread\n", __func__);
        return ret;
    }

    /* demote the cap group finally */
    ret = dsm_demote_object(src_cap_group_obj);
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to demote cap group\n", __func__);
        return ret;
    }

    dst_cap_group = (struct cap_group *)object2obj(
        dsm_get_inuse_object_by_mem_type(src_cap_group_obj, __MT_SHARED__, false));
    BUG_ON(!dst_cap_group);

    /* add the threads to the cap group */
    struct thread *src_thread, *tmp, *dst_thread;
    for_each_in_list_safe (src_thread, tmp, node, &src_cap_group->thread_list) {
        /* get the dst thread */
        dst_thread = (struct thread *)object2obj(
            dsm_get_inuse_object_by_mem_type(
                obj2object(src_thread), __MT_SHARED__, false));
        BUG_ON(!dst_thread);

        /* add the thread to the cap group */
        add_thread_to_cap_group(dst_thread, dst_cap_group);
#if 1
    // Check the validity of the dst thread
    print_thread(dst_thread);
    kprint_vmr(dst_thread->vmspace);
    kinfo("thread_ctx->tls_base_reg[TLS_FS]: %p [TLS_GS]: %p\n", dst_thread->thread_ctx->tls_base_reg[TLS_FS], dst_thread->thread_ctx->tls_base_reg[TLS_GS]);
#endif
    }

    /* TODO(FN): recycle the old cap group */

    return 0;
}

int dsm_migrate_process_restore(struct cap_group *new_cap_group)
{
    /* Restore the cap group */
    
    /* Mark all as inuse */

    reinstall_system_services(new_cap_group);
    
    return 0;
}
