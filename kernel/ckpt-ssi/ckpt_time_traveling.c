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
#include <mm/nvm.h>
#include <object/user_fault.h>

#include "ckpt_object_pool.h"
#include "ckpt_objects.h"
#include "ckpt_ws.h"

bool async_copying = false;
struct time_travel_node *latest_node = NULL;

extern struct ckpt_ws_data *init_ckpt_ws_data();
extern int recycle_create_ckpt(struct ckpt_recycle_data *recycle_data);

int init_async_copying_task(struct ckpt_obj_root *src_root_obj, u64 ckpt_name,
                            u64 name_len, u64 version_number)
{
    int r = 0;

    if (latest_node) {
        BUG_ON(latest_node->finished == false);
    } else {
        latest_node = kzalloc(sizeof(*latest_node), __SHARED__);
        if (!latest_node)
            return -ENOMEM;
    }

    async_copying = true;
    latest_node->finished = false;
    latest_node->version_number = version_number;
    latest_node->src_root_obj = src_root_obj;

    latest_node->ckpt_data = init_ckpt_ws_data();
    if (!latest_node->ckpt_data) {
        r = -ENOMEM;
        goto out_fail;
    }

    latest_node->obj_map = new_kvs(KVS_SIZE, __PRIVATE__);
    if (!latest_node->obj_map) {
        r = -ENOMEM;
        goto out_fail_free_ckpt;
    }

    /* init ckpt name */
    if (name_len > MAX_CKPT_NAME_LEN) {
        kinfo("[CKPT WS] only support name len < %d, truncked\n",
              MAX_CKPT_NAME_LEN);
        latest_node->ckpt_name_len = MAX_CKPT_NAME_LEN;
    } else
        latest_node->ckpt_name_len = name_len;

    r = copy_from_user((char *)latest_node->ckpt_name,
                       (char *)ckpt_name,
                       latest_node->ckpt_name_len);
    if (r) {
        kinfo("[INIT ASYNC] Could not copy ckpt name from user.\n");
        return r;
    }

    return r;

out_fail_free_ckpt:
    kfree(latest_node->ckpt_data);
out_fail:
    async_copying = false;
    latest_node->version_number = -1;
    return r;
}

const obj_copy_func obj_copy_tbl[TYPE_NR] = {
        [0 ... TYPE_NR - 1] = NULL,
        [TYPE_CAP_GROUP] = ckpt_cap_group_copy,
        [TYPE_THREAD] = ckpt_thread_copy,
        [TYPE_CONNECTION] = ckpt_connection_copy,
        [TYPE_NOTIFICATION] = ckpt_notification_copy,
        [TYPE_IRQ] = ckpt_irq_copy,
        [TYPE_PMO] = ckpt_pmo_copy,
        [TYPE_VMSPACE] = ckpt_vmspace_copy,
};

struct ckpt_obj_root *get_copied_obj_root(struct ckpt_obj_root *ckpt_obj_root,
                                          struct kvs *obj_map)
{
    struct ckpt_obj_root *copied_obj_root = NULL;
    struct ckpt_obj_root **copied_obj_root_ptr =
            (struct ckpt_obj_root **)kvs_get(obj_map,
                                             (kvs_key_t *)(&ckpt_obj_root));
    struct ckpt_object *src_obj, *dst_obj;
    obj_copy_func func;
    if (copied_obj_root_ptr) {
        /* object is already copyed */
        copied_obj_root = *copied_obj_root_ptr;
        // kinfo("kvs_get: obj_map=%p key=%p, value=%p\n", obj_map,
        // ckpt_obj_root, copied_obj_root);
    } else {
        // kinfo("kvs_get: obj_map=%p key=%p FAIL\n", obj_map,
        // ckpt_obj_root);

        /* get src_obj and dst_obj to be copied */
        src_obj =
                get_latest_ckpt_obj(ckpt_obj_root, latest_node->version_number);
        BUG_ON(!src_obj);

        copied_obj_root = ckpt_obj_root_alloc(FLAGS_TIME_TRAVELING);
        BUG_ON(!copied_obj_root);
        copied_obj_root->obj = ckpt_obj_root->obj;
        dst_obj = ckpt_obj_alloc(src_obj->type);
        if (!dst_obj) {
            goto out_fail;
        }
        copied_obj_root->ckpt_objs[0] = dst_obj; // always use first

        /* add ckpt_obj_root <=> copied_obj_root */
        /* must put into kvs before calling function */
        // kinfo("kvs_put: obj_map=%p, key=%p, value=%p\n", obj_map,
        // ckpt_obj_root, copied_obj_root);
        kvs_put(obj_map,
                (kvs_key_t *)(&ckpt_obj_root),
                (kvs_value_t *)(&copied_obj_root));

        /* excute copy function */
        func = obj_copy_tbl[src_obj->type];
        if (func) {
            BUG_ON(func(src_obj, dst_obj, obj_map));
        } else {
            WARN("obj copy func is NULL");
            BUG_ON(1);
        }
    }

    copied_obj_root->time_traveling = true;
    return copied_obj_root;
out_fail:
    return NULL;
}

int sys_copy_time_traveling_data()
{
    int r = 0;
    struct ckpt_obj_root *copied_ckpt_obj_root;
    struct ckpt_ws_data *data;

    if (!async_copying) {
        return r;
    }

    data = latest_node->ckpt_data;

    /* async copy */
    BUG_ON(!latest_node || !latest_node->obj_map);
    copied_ckpt_obj_root = get_copied_obj_root(latest_node->src_root_obj,
                                               latest_node->obj_map);
    if (!copied_ckpt_obj_root)
        goto out_fail;

    data->ckpt_root_obj_root = copied_ckpt_obj_root;

    /* copy recycle data and fmap pool */
    recycle_create_ckpt(&data->recycle_data);
    fmap_fault_pool_create_ckpt(&data->ckpt_fmap_fault_pool_list);

    /* put ckpt data into mapping and mark finished */
    r = ckpt_ws_put(latest_node->ckpt_data,
                    latest_node->ckpt_name,
                    latest_node->ckpt_name_len);
    if (!r) {
        goto out_fail;
    }

    async_copying = false;
    latest_node->finished = true;

out_fail:
    return r;
}
