#include <common/kvstore.h>
#include <common/errno.h>
#include <common/util.h>
#include <object/thread.h>
#include <ckpt/ckpt_data.h>
#include <ckpt/ckpt.h>
#include <arch/mmu.h>
#include <object/cap_group.h>
#include <mm/mm.h>
#include <mm/kmalloc.h>
#include <ckpt/ckpt-dsm.h>
#include <dsm/dsm-single.h>
#include <dsm/dsm-config.h>

#include "ckpt_object_pool.h"
#include "ckpt_objects.h"

static u64 obj_size[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = 0,
        [TYPE_CAP_GROUP] = sizeof(struct cap_group),
        [TYPE_THREAD] = sizeof(struct thread),
        [TYPE_CONNECTION] = sizeof(struct ipc_connection),
        [TYPE_NOTIFICATION] = sizeof(struct notification),
        [TYPE_IRQ] = sizeof(struct irq_notification),
        [TYPE_PMO] = sizeof(struct pmobject),
        [TYPE_VMSPACE] = sizeof(struct vmspace)
};

static u64 ckpt_obj_size[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = 0,
        [TYPE_CAP_GROUP] = sizeof(struct ckpt_cap_group),
        [TYPE_THREAD] = sizeof(struct ckpt_thread),
        [TYPE_CONNECTION] = sizeof(struct ckpt_ipc_connection),
        [TYPE_NOTIFICATION] = sizeof(struct ckpt_notification),
        [TYPE_IRQ] = sizeof(struct ckpt_irq_notification),
        [TYPE_PMO] = sizeof(struct ckpt_pmobject),
        [TYPE_VMSPACE] = sizeof(struct ckpt_vmspace)};

const obj_restore_func obj_restore_tbl[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = NULL,
        [TYPE_CAP_GROUP] = cap_group_restore,
        [TYPE_THREAD] = thread_restore,
        [TYPE_CONNECTION] = connection_restore,
        [TYPE_NOTIFICATION] = notification_restore,
        [TYPE_IRQ] = irq_restore,
        [TYPE_PMO] = pmo_restore,
        [TYPE_VMSPACE] = vmspace_restore,
};

struct ckpt_ws_data *init_ckpt_ws_data()
{
    struct ckpt_ws_data *data;
    data = kzalloc(sizeof(*data), __MT_SHARED__);
    if (!data) {
        return NULL;
    }
    return data;
}

struct ckpt_ws_data *get_ckpt_ws_data()
{
    struct ckpt_ws_data *data;
    extern struct ckpt_ws_data *second_latest_ws_data;
    extern struct ckpt_ws_data *latest_ws_data;
    if (likely(second_latest_ws_data)) {
        data = second_latest_ws_data;
        /* update data version number */
        data->version_number = data->version_number + 2;
    } else {
        data = init_ckpt_ws_data();
        if (!data) {
            goto out_fail;
        }

        /* initial data version number */
        if (latest_ws_data) {
            data->version_number = latest_ws_data->version_number + 1;
        } else {
            data->version_number = 1;
        }
    }

    return data;

out_fail:
    return NULL;
}

struct ckpt_object *ckpt_obj_alloc(u64 type)
{
    u64 total_size;
    struct ckpt_object *object;
    BUG_ON(type >= TYPE_NR);
    // XXX: opaque is u64 so sizeof(*object) is 8-byte aligned.
    //      Thus the address of object-defined data is always 8-byte
    //      aligned.
    total_size = sizeof(*object) + ckpt_obj_size[type];
    object = kzalloc(total_size, __MT_SHARED__);
    /* TODO: errno ecoded in pointer */
    if (!object)
        return NULL;

    object->type = type;
    return object;
}

struct ckpt_obj_root *ckpt_obj_root_alloc(int flags)
{
    struct ckpt_obj_root *root;
    root = kmalloc(sizeof(*root), __MT_SHARED__);
    if (!root)
        return NULL;

#if OBJ_OVERWRITE == 1
    root->ckpt_objs[0] = NULL;
    root->ckpt_objs[1] = NULL;
#endif
#ifdef CHCORE_SSI_SLS
    if (flags & FLAGS_CFORK) {
        root->cfork_ckpt_obj = NULL;
    }
#endif
    
    root->cow = flags & FLAGS_COW;
    root->refcnt = 0;

    if (flags & FLAGS_CFORK) {
        root->flip_flag = cfork_current_flip_flag ^ 1;
    } else {
        root->flip_flag = system_current_flip_flag ^ 1;
    }

    return root;
}

struct ckpt_obj_root *ckpt_obj_root_get(struct object *obj, int flags)
{
    struct ckpt_obj_root *root;

    if (!obj) {
        BUG("the global object map or the obj is NULL");
        return NULL;
    }

    if (flags & FLAGS_CFORK) {
        /* always allocate a new object */
        root = obj->obj_root;
        if (!root) {
            root = ckpt_obj_root_alloc(flags);
            obj->obj_root = root;
        }
        root->obj_src = obj;
        return root;
    }

    root = obj->obj_root;
    if (!root && (flags & FLAGS_ALLOC)) {
        root = ckpt_obj_root_alloc(flags);
        obj->obj_root = root;
    }

    return root;
}

#ifdef REPORT
extern int eval_obj_count[];
#endif
#ifdef REPORT
extern u64 eval_obj_time[];
extern u64 get_second_latest_obj_time;
#endif

/**
 * ckpt_obj_common: the common checkpoint function for all objects.
 * @param obj: the object to be checkpointed
 * @param ckpt_obj: the checkpoint object
 * @param flags: the flags
 * @return 0 if success, otherwise the error code
 */
static int __ckpt_obj_common(struct object *obj, 
        struct ckpt_object *ckpt_obj, int flags)
{
    int r = 0;

    switch (obj->type) {
    case TYPE_PMO: {
        r = pmo_ckpt((struct pmobject *)obj->opaque,
                     (struct ckpt_pmobject *)ckpt_obj->opaque,
                     flags);
        break;
    }
    case TYPE_CAP_GROUP: {
        r = cap_group_ckpt((struct cap_group *)obj->opaque,
                            (struct ckpt_cap_group *)ckpt_obj->opaque,
                            flags);
        break;
    }
    case TYPE_THREAD: {
        r = thread_ckpt((struct thread *)obj->opaque,
                        (struct ckpt_thread *)ckpt_obj->opaque,
                        flags);
        break;
    }
    case TYPE_CONNECTION: {
        r = connection_ckpt((struct ipc_connection *)obj->opaque,
                            (struct ckpt_ipc_connection *)ckpt_obj->opaque,
                            flags);
        break;
    }
    case TYPE_NOTIFICATION: {
        r = notification_ckpt((struct notification *)obj->opaque,
                              (struct ckpt_notification *)ckpt_obj->opaque,
                              flags);
        break;
    }
    case TYPE_IRQ: {
        r = irq_ckpt((struct irq_notification *)obj->opaque,
                     (struct ckpt_irq_notification *)ckpt_obj->opaque,
                     flags);
        break;
    }
    case TYPE_VMSPACE: {
        r = vmspace_ckpt((struct vmspace *)obj->opaque,
                         (struct ckpt_vmspace *)ckpt_obj->opaque,
                         flags);
        break;
    }
    default:
        /* Shouldn't reach here */
        BUG("Unsupported object type: %d", obj->type);
    }

    if (r) {
        kinfo("ckpt_obj_get: obj type: %d, r: %d\n", obj->type, r);
        return r;
    }

    return 0;
}

/*
 * if alloc == true, we will checkpoint the object
 */
struct ckpt_object *ckpt_obj_get(struct ckpt_obj_root *ckpt_obj_root, int flags)
{
    int r = 0;
    struct object *obj;
    struct ckpt_object *ckpt_obj = NULL;
    int current_ckpt_verison = get_current_ckpt_version();

    /* Fast pathes */
    if (flags & FLAGS_CFORK) {
        obj = ckpt_obj_root->obj_src;
        /* check whether the object has been checkpointed */
        if (ckpt_obj_root->flip_flag == cfork_current_flip_flag) {
            BUG_ON(!ckpt_obj_root->cfork_ckpt_obj);
            ckpt_obj = ckpt_obj_root->cfork_ckpt_obj;
            goto out;
        }
        /* get the ckpt_obj */
        ckpt_obj = ckpt_obj_root->cfork_ckpt_obj;
    } else {
        obj = ckpt_obj_root->obj;
        /* check whether the object has been checkpointed */
        if (system_current_flip_flag == ckpt_obj_root->flip_flag) {
            /* this object has been checkpointed */
            /* TODO(MOK): if we use COW method, we should get latest ckpt obj */
            ckpt_obj = get_second_latest_ckpt_obj(ckpt_obj_root,
                    current_ckpt_verison);
            goto out;
        }
        /* get the ckpt_obj */
        ckpt_obj = get_second_latest_ckpt_obj(
                ckpt_obj_root, current_ckpt_verison);
    }

#ifdef REPORT
    eval_obj_count[obj->type]++;
    DECLTMR;
    start();
#endif

    /* allocate ckpt_obj if it is not allocated */
    if (!ckpt_obj) {
        ckpt_obj = ckpt_obj_alloc(obj->type);
        if (!ckpt_obj) {
            CFORK_LOG_ERR("ckpt_obj_alloc failed\n");
            return NULL;
        }
    }

    /* skip checkpointing shared objects */
    if ((flags & FLAGS_CFORK) && is_cross_shared_obj(obj)) {
        ckpt_obj_root->cross_shared = true;
        /* for cross-shared object, we have the same obj_dst and obj_src */
        ckpt_obj_root->obj_dst = obj;
        goto out;
    }

    /* set ckpt_obj_root->ckpt_objs and flip_flag */
    if (flags & FLAGS_CFORK) {
        ckpt_obj_root->cfork_ckpt_obj = ckpt_obj;
        ckpt_obj_root->flip_flag = cfork_current_flip_flag;
    } else {
        set_second_latest_ckpt_obj(
            ckpt_obj_root, current_ckpt_verison, ckpt_obj);
        ckpt_obj_root->flip_flag = system_current_flip_flag;
    }

    /* increase the refcnt of the ckpt_obj_root */
    ckpt_obj_root->refcnt++;

#ifdef REPORT
    u64 object_time_begin = plat_get_mono_time();
    get_second_latest_obj_time += object_time_begin - timer_start;
#endif

    /* checkpoint the object */
    if ((r = __ckpt_obj_common(obj, ckpt_obj, flags)) != 0) {
        return NULL;
    }

#ifdef REPORT
    if (obj->type != TYPE_CAP_GROUP) {
        eval_obj_time[obj->type] += plat_get_mono_time() - object_time_begin;
    }
#endif
out:
    BUG_ON(!ckpt_obj);
    CFORK_LOG_DEBUG("%s: ckpt_obj_root: %p obj: %p, type: %s, cross-shared: %d\n", 
        __func__, ckpt_obj_root, obj, obj_name_tbl[obj->type], ckpt_obj_root->cross_shared);
    return ckpt_obj;
}

inline struct object *
restore_obj_get(struct ckpt_obj_root *ckpt_obj_root, int flags)
{
    if (flags & FLAGS_CFORK) {
        return ckpt_obj_root->obj_dst;
    } else {
        return ckpt_obj_root->obj;
    }
}

/*
 * if alloc == true, we will restore the object
 */
struct object *restore_obj_get_by_cap_group(struct ckpt_obj_root *ckpt_obj_root,
                                            struct kvs *obj_map, int flags)
{
    obj_restore_func func;
    struct object *obj = NULL;
    struct ckpt_object *ckpt_obj;
    struct object **obj_value =
            (struct object **)kvs_get(obj_map, (kvs_key_t *)&ckpt_obj_root);

    if (obj_value) {
        /* object is already allocated */
        obj = *obj_value;
        goto out;
    }

    // get ckpt obj
    if (flags & FLAGS_TIME_TRAVELING) {
        ckpt_obj = ckpt_obj_root->ckpt_objs[0];
        obj = ckpt_obj_root->obj;
    } else if (flags & FLAGS_CFORK) {
        ckpt_obj = ckpt_obj_root->cfork_ckpt_obj;
        obj = ckpt_obj_root->obj_dst;
    } else {
        ckpt_obj = get_second_latest_ckpt_obj(
            ckpt_obj_root, get_current_ckpt_version());
        obj = ckpt_obj_root->obj;
    }

    // allocate object
    if (flags & FLAGS_CFORK) {
        // a object that is cross-shared not do requie allocation
        if (ckpt_obj_root->cross_shared == true) {
            BUG_ON(!obj);
            goto out;
        } else {
            // always allocate a new object for CFORK
            BUG_ON(!ckpt_obj);
            obj = object_alloc(ckpt_obj->type, obj_size[ckpt_obj->type], __MT_SHARED__);
            BUG_ON(!obj);
            obj->obj_root = ckpt_obj_root;
            ckpt_obj_root->obj_dst = obj;
        }
    } else {
        // allocate object if not allocated
        if (!obj && (flags & FLAGS_ALLOC)) {
            obj = object_alloc(ckpt_obj->type, obj_size[ckpt_obj->type], __MT_SHARED__);
            BUG_ON(!obj);
            obj->obj_root = ckpt_obj_root;
            ckpt_obj_root->obj = obj;
        }
    }

    // put obj into obj map
    kvs_put(obj_map, (kvs_key_t *)&ckpt_obj_root, (kvs_value_t *)(&obj));

#ifdef RESTORE_REPORT
    DECLTMR;
    start();
#endif

    /* Invoke the object-specific restore routine */
    func = obj_restore_tbl[obj->type];

    if (func) {
        BUG_ON(func(obj, ckpt_obj, obj_map, flags));
    } else {
        BUG("obj restore func is NULL");
    }

#ifdef RESTORE_REPORT
    if (obj->type != TYPE_CAP_GROUP)
        eval_restore_obj_time[obj->type] += stop();
    eval_restore_obj_count[obj->type]++;
#endif

out:
    CFORK_LOG_DEBUG("%s: obj: %p, type: %s, cross-shared: %d\n", 
        __func__, obj, obj_name_tbl[obj->type], ckpt_obj_root->cross_shared);
    return obj;
}

inline struct ckpt_object *
get_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root, u64 version_number)
{
    return ckpt_obj_root->ckpt_objs[version_number % 2];
}

inline struct ckpt_object *
get_second_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root,
                           u64 version_number)
{
    return ckpt_obj_root->ckpt_objs[1 - version_number % 2];
}

inline void set_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root,
                                u64 version_number,
                                struct ckpt_object *ckpt_obj)
{
    ckpt_obj_root->ckpt_objs[version_number % 2] = ckpt_obj;
}

inline void set_second_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root,
                                       u64 version_number,
                                       struct ckpt_object *ckpt_obj)
{
    ckpt_obj_root->ckpt_objs[1 - version_number % 2] = ckpt_obj;
}
