#include <object/object.h>
#include <common/types.h>
#include <dsm/tiering.h>

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
    if (obj->status != DSM_STATUS_INUSE) {
        ret = -EINVAL;
        goto out_ret;
    }

    if (!try_lock(&obj->tiering_lock)) {
        ret = -EAGAIN;
        goto out_unlock;
    }

    /* check again to avoid race */
    if (obj->status != DSM_STATUS_INUSE) {
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
        return -EINVAL;
    }

    ret = __dsm_tiering_start_migration(obj);
    if (ret == -EINVAL && obj->status == DSM_STATUS_MIGRATED) {
        /* already migrated; demote is done by other thread */
        return 0;
    } else if (ret) {
        return ret;
    } // else ret == 0; start migration

    /* Malloc shared object */
    target = obj->pair_obj;
    if (!target) {
        target = kmalloc(sizeof(struct object), __MT_SHARED__);
        if (!target) {
            return -ENOMEM;
        }
        obj->pair_obj = target;
    } else {
        /* already allocated target, should make sure it is invalid */
        if (target->status != DSM_STATUS_INVALID) {
            return -EAGAIN;
        }
    }

    /* copy data from obj to target */
    dsm_copy_func func = dsm_copy_tbl[obj->type];
    if (!func) {
        return -EINVAL;
    }
    func(obj, target);

    /* Enable shared object */
    ret = __dsm_tiering_finish_migration(obj);
    if (ret) {
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
    kwarn("dsm_promote_object: not implemented\n");
    return -ENOSYS;
}
