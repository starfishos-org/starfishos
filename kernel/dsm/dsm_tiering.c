#include <object/object.h>
#include <object/cap_group.h>
#include <common/types.h>
#include <dsm/tiering.h>
#include <dsm/dsm-config.h>

#include "dsm_tiering.h"

const dsm_copy_func dsm_copy_tbl[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = NULL,
        [TYPE_CAP_GROUP] = dsm_copy_cap_group,
        [TYPE_THREAD] = dsm_copy_thread,
        [TYPE_CONNECTION] = dsm_copy_connection,
        [TYPE_NOTIFICATION] = dsm_copy_notification,
        [TYPE_IRQ] = dsm_copy_irq,
        [TYPE_PMO] = dsm_copy_pmo,
        [TYPE_VMSPACE] = dsm_copy_vmspace,
};

static int __dsm_tiering_start_migration(struct object *obj)
{
    int ret = 0;

    /* already migrating or not in use */
    if (unlikely(obj->status != DSM_STATUS_INUSE)) {
        ret = -EINVAL;
        goto out_ret;
    }

    if (try_lock(&obj->tiering_lock) != 0) {
        ret = -EAGAIN;
        goto out_ret;
    }

    /* check again to avoid race */
    if (unlikely(obj->status != DSM_STATUS_INUSE)) {
        ret = -EINVAL;
        goto out_unlock;
    }

    obj->status = DSM_STATUS_MIGRATING;

out_unlock:
    unlock(&obj->tiering_lock);
out_ret:
    return ret;
}

static inline int __dsm_tiering_finish_migration(struct object *obj)
{
    struct object *target = obj->pair_obj;
    if (!target || obj->status != DSM_STATUS_MIGRATING 
        || target->status != DSM_STATUS_INVALID) {
        return -EINVAL;
    }
    if (obj->dirty_bit) {
        /* TODO: flush dirty pages */
        return -EAGAIN;
    }
    /* nobody should be using the obj or target here */
    obj->status = DSM_STATUS_MIGRATED;
    target->status = DSM_STATUS_INUSE;
    return 0;
}

/**
 * @brief demote the object to the lower tier
 * the object can be already put on the lower tier,
 * so we don't need to do anything
 * @param obj 
 * @return int 
 */
int dsm_demote_object(struct object *obj)
{
    struct object *target;
    int ret = 0;

    /* Check object is real private */
    if (!is_private_object(obj)) {
        DSM_TIER_LOG_DEBUG("obj is shared object\n");
        return 0;
    }

    /* Already migrated; demote is done by other thread */
    if (obj->status == DSM_STATUS_MIGRATED) {
        BUG_ON(!obj->pair_obj || obj->pair_obj->status != DSM_STATUS_INUSE);
        DSM_TIER_LOG_DEBUG("%s: obj (type %s) is already migrated\n", 
            __func__, obj_name_tbl[obj->type]);
        return 0;
    }

    /* A system services object that should not be migrated */
    if (is_system_services_object(obj)) {
        target = dsm_get_object_by_mem_type(obj, __MT_SHARED__, true);
        return !target ? -ENOMEM : 0;
    }

    /* Start migration */
    if ((ret = __dsm_tiering_start_migration(obj)) != 0) {
        DSM_TIER_LOG_ERR("%s: failed to start migration\n", __func__);
        return ret;
    } // else ret == 0; start migration

    /* Malloc shared object */
    if (!obj->pair_obj) {
        if ((ret = dsm_alloc_pair_object(obj, __MT_SHARED__)) != 0) {
            DSM_TIER_LOG_ERR("%s: failed to alloc pair object\n", __func__);
            return ret;
        }
    }

    target = obj->pair_obj;
    BUG_ON(!target);

    DSM_TIER_LOG_DEBUG("demote object %p, target %p, type %s\n", 
        obj, target, obj_name_tbl[obj->type]);


copy_again:
    /* clear dirty bit first */
    obj->dirty_bit = 0;

    /* copy data from obj to target */
    dsm_copy_func func = dsm_copy_tbl[obj->type];
    if (!func) {
        DSM_TIER_LOG_ERR("dsm tiering copy function not found\n");
        return -EINVAL;
    }
    func(obj, target);

    /* Enable shared object */
    ret = __dsm_tiering_finish_migration(obj);
    if (ret == -EAGAIN) {
        /* clear dirty bit and try to copy the object again */
        goto copy_again;
    } else if (ret) {
        return ret;
    }

    return 0;
}

/**
 * @brief promote the object to the higher tier
 * the object can be already put on the higher tier,
 * so we don't need to do anything
 * @param obj 
 * @return int 
 */

int dsm_promote_object(struct object *obj)
{
    struct object *target;
    int ret = 0;

    /* Check object is real private */
    if (!is_shared_object(obj)) {
        DSM_TIER_LOG_DEBUG("obj is not a shared object\n");
        return 0;
    }

    if (obj->status == DSM_STATUS_MIGRATED) {
        /* already migrated; demote is done by other thread */
        if (obj->pair_obj && is_private_object(obj->pair_obj) && 
            obj->pair_obj->status == DSM_STATUS_INUSE) {
            DSM_TIER_LOG_DEBUG("%s: obj (type %s) is already migrated\n", 
                __func__, obj_name_tbl[obj->type]);
            return 0;
        }
    }

    if ((ret = __dsm_tiering_start_migration(obj)) != 0) {
        DSM_TIER_LOG_ERR("%s: failed to start migration\n", __func__);
        return ret;
    } // else ret == 0; start migration

    /* Malloc shared object */
    if (!obj->pair_obj) {
        if ((ret = dsm_alloc_pair_object(obj, __MT_SHARED__)) != 0) {
            DSM_TIER_LOG_ERR("%s: failed to alloc pair object\n", __func__);
            return ret;
        }
    } else {
        DSM_TIER_LOG_WARN("%s: should free it!! but this object is not allocated on this machine\n", __func__);
        // TODO: should free it! but this object is not allocated on this machine
    }

    target = obj->pair_obj;
    BUG_ON(!target);

    DSM_TIER_LOG_DEBUG("promote object %p, target %p, type %s\n", 
        obj, target, obj_name_tbl[obj->type]);

copy_again:
    /* clear dirty bit first */
    obj->dirty_bit = 0;

    /* copy data from obj to target */
    dsm_copy_func func = dsm_copy_tbl[obj->type];
    if (!func) {
        DSM_TIER_LOG_ERR("dsm tiering copy function not found\n");
        return -EINVAL;
    }
    func(obj, target);

    /* Enable shared object */
    ret = __dsm_tiering_finish_migration(obj);
    if (ret == -EAGAIN) {
        /* clear dirty bit and try to copy the object again */
        goto copy_again;
    } else if (ret) {
        return ret;
    }

    return 0;
}

int demote_each_object_in_cap_group(struct cap_group *cap_group, u64 type_mask)
{
    struct slot_table *slot_table = &cap_group->slot_table;
    int slot_id;
    int ret = 0;
    for_each_set_bit (slot_id, slot_table->slots_bmp, slot_table->slots_size) {
        struct object_slot *slot = slot_table->slots[slot_id];
        BUG_ON(!slot);
        struct object *object = slot->object;
        BUG_ON(!object);

        if (!(type_mask & (1L << (object->type)))) {
            continue;
        }

        // kinfo("try to demote object %p type: %d\n", object, object->type);
        ret = dsm_demote_object(object);
        if (ret) {
            DSM_TIER_LOG_DEBUG("Failed to demote object %p\n", object);
            return ret;
        }
    }
    return 0;
}

int promote_each_object_in_cap_group(struct cap_group *cap_group, u64 type_mask)
{
    struct slot_table *slot_table = &cap_group->slot_table;
    int slot_id;
    int ret = 0;
    for_each_set_bit (slot_id, slot_table->slots_bmp, slot_table->slots_size) {
        struct object_slot *slot = slot_table->slots[slot_id];
        BUG_ON(!slot);
        struct object *object = slot->object;
        BUG_ON(!object);

        if (!(type_mask & (1L << (object->type)))) {
            continue;
        }

        kinfo("try to promote object %p type: %d\n", object, object->type);
        ret = dsm_promote_object(object);
        if (ret) {
            DSM_TIER_LOG_DEBUG("Failed to promote object %p\n", object);
            return ret;
        }
    }
    return 0;
}
