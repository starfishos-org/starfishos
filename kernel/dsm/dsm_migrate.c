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
    
    /* Check the memory of process first */
    // flags = ~((1L << TYPE_CAP_GROUP) | (1L << TYPE_THREAD));
    // flags = ~((1L << TYPE_THREAD) | (1L << TYPE_CONNECTION) | (1L << TYPE_NOTIFICATION));
    flags = ~((1L << TYPE_THREAD) | (1L << TYPE_CAP_GROUP));
    /* demote all objects except cap group and thread */
    ret = demote_each_object_in_cap_group((struct cap_group *)object2obj(root_cg_obj), flags);
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to demote objects in cap group %p\n", __func__, root_cg_obj);
        return ret;
    }

    return 0;
}

extern int stop_connection(struct ipc_connection *conn);
extern int stop_notification(struct notification *notifc);

int stop_all_connections(struct cap_group *cap_group)
{
    struct slot_table *slot_table = &cap_group->slot_table;
    int slot_id;
    int ret = 0;
    for_each_set_bit (slot_id, slot_table->slots_bmp, slot_table->slots_size) {
        struct object_slot *slot = slot_table->slots[slot_id];
        BUG_ON(!slot);
        struct object *object = slot->object;
        BUG_ON(!object);

        if (object->type == TYPE_CONNECTION) {
            ret = stop_connection((struct ipc_connection *)object->opaque);
            if (ret) {
                DSM_TIER_LOG_ERR("%s: failed to stop connection %p\n", __func__, object);
                return ret;
            }
        } else if (object->type == TYPE_NOTIFICATION) {
            ret = stop_notification((struct notification *)object->opaque);
            if (ret) {
                DSM_TIER_LOG_ERR("%s: failed to stop notification %p\n", __func__, object);
                return ret;
            }
        }
    }

    return 0;
}

int dsm_migrate_process_ckpt(struct object *src_cap_group_obj)
{
    int ret = 0, flags;
    struct cap_group *src_cap_group, *dst_cap_group;

    src_cap_group = (struct cap_group *)object2obj(src_cap_group_obj);

    flags = (1L << TYPE_THREAD);

    /* demote the threads */
    ret = demote_each_object_in_cap_group(src_cap_group, flags);
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
        dsm_get_object_by_mem_type(src_cap_group_obj, __MT_SHARED__, false));
    BUG_ON(!dst_cap_group);

    /* add the threads to the cap group */
    struct thread *src_thread, *tmp, *dst_thread;
    for_each_in_list_safe (src_thread, tmp, node, &src_cap_group->thread_list) {
        /* get the dst thread */
        dst_thread = (struct thread *)object2obj(
            dsm_get_object_by_mem_type(
                obj2object(src_thread), __MT_SHARED__, false));
        BUG_ON(!dst_thread);

        /* add the thread to the cap group */
        add_thread_to_cap_group(dst_thread, dst_cap_group);
#if 0
    // Check the validity of the dst thread
    print_thread(dst_thread);
    kprint_vmr(dst_thread->vmspace);
    memcpy(dst_cap_group->cap_group_name, "restored", 10);
    kinfo("src TLS_FS: %p [TLS_GS]: %p\n", src_thread->thread_ctx->tls_base_reg[TLS_FS], src_thread->thread_ctx->tls_base_reg[TLS_GS]);
    kinfo("dst TLS_FS: %p [TLS_GS]: %p\n", dst_thread->thread_ctx->tls_base_reg[TLS_FS], dst_thread->thread_ctx->tls_base_reg[TLS_GS]);
#endif
    }

    /* TODO(FN): recycle the old cap group */

    return 0;
}

extern void arch_vmspace_init(struct vmspace *);
int dsm_migrate_process_restore(struct cap_group *new_cap_group)
{
    struct vmspace *vmspace;

    /* Restore the cap group */
    
    /* Mark all as inuse */
    int i;
    struct slot_table *slot_table = &new_cap_group->slot_table;
    struct object *object;
    for_each_set_bit(i, slot_table->slots_bmp, slot_table->slots_size) {
        if (!slot_table->slots[i]) {
            BUG("slot is NULL while bmp is not, slot id: %d\n", i);
        }
        object = slot_table->slots[i]->object;
        // TODO(FN): check if it is a bridge object
        if (object->type != TYPE_THREAD) {
            object->status = DSM_STATUS_INUSE;
            object->pair_obj = NULL;
        }
        // DSM_TIER_LOG_DEBUG("[table=%p] restore slot: ID %d, object: %p, type: %s\n", 
        //     slot_table, i, object, obj_name_tbl[object->type]);
    }

    /* Re-init vmspace*/
    vmspace = (struct vmspace *)obj_get(new_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    BUG_ON(!vmspace);
    arch_vmspace_init(vmspace);

    reinstall_system_services(new_cap_group);
    
    return 0;
}
