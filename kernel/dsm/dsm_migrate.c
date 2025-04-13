#include <dsm/tiering.h>
#include <object/cap_group.h>
#include <common/list.h>
#include <object/thread.h>
#include <mm/vmspace.h>
#include <object/object.h>
#include <dsm/dsm-single.h>
#include <common/radix.h>
#include "dsm_tiering.h"

#ifdef PERF_TIMING_CFORK
extern u64 perf_cfork_time[PERF_CFORK_TYPE_NR];
extern u64 perf_dsm_copy_time[TYPE_NR];
#endif

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
    // flags = ~((1L << TYPE_THREAD) | (1L << TYPE_CAP_GROUP));
    flags = 0xFFFFFFFF;
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
#ifdef PERF_TIMING_CFORK
    u64 start_time = perf_timing_get_time(), end_time;
#endif
    int ret = 0;
    struct cap_group *src_cap_group, *dst_cap_group;
    struct object *dst_cap_group_obj;

    src_cap_group = (struct cap_group *)object2obj(src_cap_group_obj);
    
    /* ckpt the threads */
    ret = ckpt_each_object_in_cap_group(src_cap_group,  (1L << TYPE_THREAD));
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to ckpt thread\n", __func__);
        return ret;
    }

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_CKPT_THREADS] += end_time - start_time;
    start_time = end_time;
#endif

    /* ckpt the cap group finally */
    dst_cap_group_obj = dsm_get_object_by_mem_type(src_cap_group_obj, __MT_SHARED__, false);
    BUG_ON(!dst_cap_group_obj);

    ret = dsm_ckpt_cap_group(src_cap_group_obj, dst_cap_group_obj);
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to ckpt cap group\n", __func__);
        return ret;
    }

    dst_cap_group = (struct cap_group *)object2obj(dst_cap_group_obj);
    BUG_ON(!dst_cap_group);

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_CKPT_CAP_GROUP] += end_time - start_time;
    start_time = end_time;
#endif

    /* add the threads to the cap group */
    struct thread *src_thread, *tmp, *dst_thread;
    struct object *src_thread_obj, *dst_thread_obj;
    for_each_in_list_safe (src_thread, tmp, node, &src_cap_group->thread_list) {
        /* get the dst thread */
        src_thread_obj = obj2object(src_thread);
        dst_thread_obj = dsm_get_object_by_mem_type(src_thread_obj, __MT_SHARED__, false);
        BUG_ON(!dst_thread_obj);

        dst_thread = (struct thread *)object2obj(dst_thread_obj);
        BUG_ON(!dst_thread);

        /* add the thread to the cap group */
        add_thread_to_cap_group(dst_thread, dst_cap_group);

        /* free old thread */
        list_del(&src_thread->node);

        // BUG_ON(src_thread_obj->refcount > 1);
        // obj_put(src_thread_obj);
        src_thread_obj->refcount = 0;
        object_free(src_thread_obj);
        dst_thread_obj->pair_obj = NULL;

#if 0
    // Check the validity of the dst thread
    print_thread(dst_thread);
    kprint_vmr(dst_thread->vmspace);
    memcpy(dst_cap_group->cap_group_name, "restored", 10);
    kinfo("src TLS_FS: %p [TLS_GS]: %p\n", src_thread->thread_ctx->tls_base_reg[TLS_FS], src_thread->thread_ctx->tls_base_reg[TLS_GS]);
    kinfo("dst TLS_FS: %p [TLS_GS]: %p\n", dst_thread->thread_ctx->tls_base_reg[TLS_FS], dst_thread->thread_ctx->tls_base_reg[TLS_GS]);
#endif
    }

#ifdef PERF_TIMING_CFORK
    end_time = perf_timing_get_time();
    perf_cfork_time[PERF_CFORK_CKPT_THREADS] += end_time - start_time;
    start_time = end_time;
#endif
    /* TODO(FN): recycle the old cap group */

    return 0;
}

static int dsm_promote_thread(struct object *thread_obj, struct cap_group *new_cap_group)
{
    int ret = 0;
    struct object *thread_new_object;
    struct thread *new_thread;

    ret = dsm_promote_object(thread_obj);
    if (ret) {
        DSM_TIER_LOG_ERR("%s: failed to promote the thread: %p", __func__, thread_obj);
        return ret;
    }

    thread_new_object = dsm_get_object_by_mem_type(thread_obj, __MT_PRIVATE__, false);
    BUG_ON(!thread_new_object);
    new_thread = (struct thread *)object2obj(thread_new_object);
    BUG_ON(!new_thread);
    if (unlikely(new_thread->thread_ctx->affinity != NO_AFF)) {
        new_thread->thread_ctx->affinity = NO_AFF;
    }

    /* add the thread to the cap group */
    new_thread->cap_group = new_cap_group;
    BUG_ON(new_thread->vmspace == NULL || new_thread->vmspace->pgtbl == NULL);

    return 0;
}

extern void arch_vmspace_init(struct vmspace *);
int dsm_migrate_process_restore(struct cap_group *new_cap_group)
{
    struct vmspace *vmspace;
    int i, ret = 0;
    struct slot_table *slot_table = &new_cap_group->slot_table;
    struct object *object, *promoted_object;

    for_each_set_bit(i, slot_table->slots_bmp, slot_table->slots_size) {
        if (!slot_table->slots[i]) {
            BUG("slot is NULL while bmp is not, slot id: %d\n", i);
        }
        object = slot_table->slots[i]->object;
        // TODO(FN): check if it is a bridge object
        DSM_TIER_LOG_DEBUG("object: %p dsm_type: %d type: %d\n", object, object->dsm_type, object->type);
        if (object->dsm_type == DSM_TYPE_NORMAL || object->dsm_type == DSM_TYPE_BRIDGE) {
            object->status = DSM_STATUS_INUSE;
            object->pair_obj = NULL;
        }

        if (object->type == TYPE_THREAD && object->dsm_type == DSM_TYPE_BRIDGE) {
            // promote the thread
            ret = dsm_promote_thread(object, new_cap_group);
            if (ret) {
                DSM_TIER_LOG_ERR("%s: failed to promote the thread: %p", 
                        __func__, object);
                return -EINVAL;
            }
            promoted_object = dsm_get_object_by_mem_type(object, __MT_PRIVATE__, false);
            BUG_ON(!promoted_object);
            promoted_object->status = DSM_STATUS_INUSE;
            object->status = DSM_STATUS_MIGRATED;

            // update the link to the promoted thread
            // 1. update in the slot table
            slot_table->slots[i]->object = promoted_object;
        }
    }

    // 2. update the thread list
    struct thread *thread, *tmp;
    for_each_in_list_safe (thread, tmp, node, &new_cap_group->thread_list) {
        object = obj2object(thread);
        promoted_object = dsm_get_object_by_mem_type(object, __MT_PRIVATE__, false);
        BUG_ON(!promoted_object);

        list_del(&thread->node);
        list_add(&(((struct thread *)object2obj(promoted_object))->node), 
            &new_cap_group->thread_list);
    }

    /* Re-init vmspace*/
    vmspace = (struct vmspace *)obj_get(new_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    BUG_ON(!vmspace);
    arch_vmspace_init(vmspace);

    reinstall_system_services(new_cap_group);
    
    return 0;
}

void dsm_check_pa_on_shared_memory(vaddr_t va, paddr_t pa)
{
    if (!IS_SHM_PADDR(pa)) {
        printk("pa %p is not on shared memory\n", pa);
        return;
    }
}

int check_thread_ready_to_run_across_machines(struct thread *thread)
{
    struct cap_group *cap_group = thread->cap_group;

    if (cap_group->is_cross_machine == false) {
        printk("cap_group is not cross machine: %s\n", cap_group->cap_group_name);
        return 1;
    }

    struct slot_table *slot_table = &cap_group->slot_table;
    int slot_id;
    read_lock(&slot_table->table_guard);
    for_each_set_bit(slot_id, slot_table->slots_bmp, slot_table->slots_size) {
        struct object_slot *slot = slot_table->slots[slot_id];
        BUG_ON(!slot);
        struct object *object = slot->object;
        BUG_ON(!object);
        if (object->mem_type != __MT_SHARED__) {
            printk("object not shared: %p type: %s\n", object, obj_name_tbl[object->type]);
            return 1;
        }
        switch (object->type) {
            case TYPE_THREAD:
            {
                struct thread *thread = (struct thread *)object2obj(object);
                if (!IS_SHM_PADDR(virt_to_phys(thread->thread_ctx))) {
                    printk("thread context is not shared: %p\n", object);
                    return 1;
                }
                break;
            }
            case TYPE_PMO:
            {
                struct pmobject *pmo = (struct pmobject *)object2obj(object);
                if (pmo->mm_type != __MT_SHARED__) {
                    printk("pmo %p is not shared, mm_type = %d type = %d\n", 
                        pmo, pmo->mm_type, pmo->type);
                    return 1;
                }
                if (is_radix_pmo(pmo)) {
                    // loop the radix tree and check paddr of each page
                    struct radix *radix = pmo->radix;
                    radix_traverse(radix, dsm_check_pa_on_shared_memory);
                } else if (is_continuous_pmo(pmo)) {
                    if (!IS_SHM_PADDR(pmo->start)) {
                        printk("pmo %p is not shared, start = %p\n", pmo, pmo->start);
                        return 1;
                    }
                }
                break;
            }
            case TYPE_VMSPACE:
            {
                struct vmspace *vmspace = (struct vmspace *)object2obj(object);
                if (!IS_SHM_PADDR(virt_to_phys(vmspace->pgtbl))) {
                    printk("page table is not shared: %p\n", object);
                    return 1;
                }
                break;
            }
            default:
                break;
        }
    }
    read_unlock(&slot_table->table_guard);
    return 0;
}