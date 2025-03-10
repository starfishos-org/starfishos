#include <object/thread.h>
#include <ckpt/ckpt_data.h>
#include <common/kvstore.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <object/cap_group.h>
#include <mm/mm.h>
#include <ckpt/ckpt-dsm.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <ckpt/ckpt.h>
#include <sched/context.h>
#include <irq/timer.h>

#include "ckpt_ws.h"
#include "ckpt_object_pool.h"
#include "ckpt_objects.h"

extern struct irq_notification *irq_notifcs[128];
extern u8 irq_handle_type[128];

extern struct ckpt_ws_data *second_latest_ws_data;
extern struct ckpt_ws_data *latest_ws_data;

extern int recycle_restore(struct ckpt_recycle_data *recycle_data,
                           struct kvs *obj_map);
extern int fmap_fault_pool_restore(struct list_head *ckpt_fmap_fault_pool_list,
                                   struct kvs *obj_map);

// TODO: need support more flexable request
int sys_whole_restore(u64 ckpt_name, u64 name_len)
{
    struct ckpt_ws_data *data;
    struct object *obj;
    struct ckpt_obj_root *ckpt_obj_root;
    struct kvs *obj_map;
    struct ckpt_ws_info *info;
    void *name;
    int r;
    bool ckpt_initialized = CKPT_INITIALIZED;
    CKPT_INITIALIZED = false;

    /* stop all cpus by sending ipis to all remote cpus */
    sys_ipi_stop_all();

#ifdef RESTORE_REPORT
    for (int i = 0; i < TYPE_NR; i++) {
        eval_restore_obj_time[i] = 0;
        eval_restore_obj_count[i] = 0;
    }
#endif

    system_current_flip_flag = 0;

    if (ckpt_name && name_len) {
        name = kmalloc(name_len, __SHARED__);
        if (!name) {
            r = -ENOMEM;
            goto out_fail;
        }
        r = copy_from_user((char *)name, (char *)ckpt_name, name_len);
        if (r) {
            kinfo("[RESTORE] Could not copy ckpt name from user.\n");
            return r;
        }
        info = ckpt_ws_query_by_name((char *)name, name_len);
        if (!info) {
            kinfo("[RESTORE] Could not find ckpt by name.\n");
            r = -ENOENT;
        }
        data = ckpt_ws_get((u64)info);
    } else {
        data = ckpt_ws_get_latest();
    }

    if (!data) {
        kinfo("[RESTORE] Could not find ckpt.\n");
        r = -ENODATA;
        goto out_fail;
    }

    ckpt_obj_root = data->ckpt_root_obj_root;

    obj_map = new_kvs(KVS_SIZE, __PRIVATE__);
    if (!obj_map) {
        r = -ENOMEM;
        goto out_fail;
    }
    set_current_ckpt_version(data->version_number);

    /* clear the sched queues first */
    sched_clear();

    /* restore root_cap_group obj */
    obj = restore_obj_get_by_cap_group(ckpt_obj_root, obj_map, FLAGS_ALLOC);

    root_cap_group_obj_for_ckpt = obj;
    root_cap_group = (struct cap_group *)(obj->opaque);

    recycle_restore(&data->recycle_data, obj_map);
    fmap_fault_pool_restore(&data->ckpt_fmap_fault_pool_list, obj_map);

    /* free the tmp obj map */
    kfree(obj_map);

    if (!data->ckpt_root_obj_root->time_traveling) {
        second_latest_ws_data = NULL;
        latest_ws_data = data;
    }
#ifdef RESTORE_REPORT
    int tcnt = 0;
    for (int i = 0; i < TYPE_NR; i++) {
        printk("object count %d: %d, time: %lu\n",
               i,
               eval_restore_obj_count[i],
               eval_restore_obj_time[i]);
        tcnt += eval_restore_obj_count[i];
    }
    printk("tcnt: %d\n", tcnt);
#endif
    sys_ipi_start_all();
    CKPT_INITIALIZED = ckpt_initialized;

    return 0;

/* continue all cpus by sending ipis to all remote cpus */
out_fail:
    sys_ipi_start_all();
    return r;
}

int sys_whole_restore_without_ipi(u64 ckpt_name, u64 name_len)
{
    struct ckpt_ws_data *data;
    struct object *obj;
    struct ckpt_obj_root *ckpt_obj_root;
    struct kvs *obj_map;
    struct ckpt_ws_info *info;
    void *name;
    int r;
    bool ckpt_initialized = CKPT_INITIALIZED;
    CKPT_INITIALIZED = false;
    smp_mb();

#ifdef RESTORE_REPORT
    for (int i = 0; i < TYPE_NR; i++) {
        eval_restore_obj_time[i] = 0;
        eval_restore_obj_count[i] = 0;
    }
#endif

    system_current_flip_flag = 0;
    // printk("before restore:free mem size: %u\n",get_free_mem_size());
    if (ckpt_name && name_len) {
        name = kmalloc(name_len, __SHARED__);
        if (!name) {
            r = -ENOMEM;
            goto out_fail;
        }
        r = copy_from_user((char *)name, (char *)ckpt_name, name_len);
        if (r) {
            kinfo("[RESTORE] Could not copy ckpt name from user.\n");
            return r;
        }
        info = ckpt_ws_query_by_name((char *)name, name_len);
        if (!info) {
            kinfo("[RESTORE] Could not find ckpt by name.\n");
            r = -ENOENT;
        }
        data = ckpt_ws_get((u64)info);
    } else {
        data = ckpt_ws_get_latest();
    }

    if (!data) {
        kinfo("[RESTORE] Could not find ckpt.\n");
        r = -ENODATA;
        goto out_fail;
    }

    ckpt_obj_root = data->ckpt_root_obj_root;
    // ckpt_obj_map = data->map;

    obj_map = new_kvs(KVS_SIZE, __PRIVATE__);
    if (!obj_map) {
        r = -ENOMEM;
        goto out_fail;
    }
    set_current_ckpt_version(data->version_number);
    /* restore root_cap_group obj */
    obj = restore_obj_get_by_cap_group(ckpt_obj_root, obj_map, true);
    root_cap_group_obj_for_ckpt = obj;
    root_cap_group = (struct cap_group *)(obj->opaque);

    recycle_restore(&data->recycle_data, obj_map);
    fmap_fault_pool_restore(&data->ckpt_fmap_fault_pool_list, obj_map);

    second_latest_ws_data = NULL;
    latest_ws_data = data;
#ifdef RESTORE_REPORT
    int tcnt = 0;
    for (int i = 0; i < TYPE_NR; i++) {
        printk("object count %d: %d, time: %lu\n",
               i,
               eval_restore_obj_count[i],
               eval_restore_obj_time[i]);
        tcnt += eval_restore_obj_count[i];
    }
    printk("tcnt: %d\n", tcnt);
#endif
    CKPT_INITIALIZED = ckpt_initialized;

    return 0;

/* TODO: continue all cpus by sending ipis to all remote cpus */
/* TODO: free all we allocate */
out_fail:
    return r;
}
