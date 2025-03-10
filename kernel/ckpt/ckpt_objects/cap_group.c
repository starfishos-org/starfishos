#include "../ckpt_objects.h"
#include "../ckpt_object_pool.h"

#ifdef REPORT
extern u64 eval_obj_time[];
#endif

int slot_table_ckpt(struct cap_group *cap_group,
                    struct ckpt_cap_group *ckpt_cap_group)
{
#ifdef REPORT
    DECLTMR;
    start();
#endif
    struct slot_table *slot_table;
    int bmp_long_count;
    unsigned long map;
    int ckpt_table_size;
    int r, i, offset, count;

    slot_table = &cap_group->slot_table;
    bmp_long_count = BITS_TO_LONGS(slot_table->slots_size);
    ckpt_table_size = 0;

    /* Calculate number of objects to be stored */
    /* TODO: optimize it to bit-count instruction or bit-count algorithm */
    for (i = 0; i < bmp_long_count; i++) {
        /* Traverse these 64 slots */
        for (offset = 0, map = slot_table->slots_bmp[i]; map;
             offset++, map >>= 1) {
            if (map & 1) {
                ckpt_table_size++;
            }
        }
    }

    /*
     * Allocate space for ckpt slot table, omitting 1st cap.
     * Because 1st cap is cap_group itself
     */
    ckpt_cap_group->table_size = ckpt_table_size - 1;
    ckpt_cap_group->slots_size = slot_table->slots_size;

    if (ckpt_cap_group->slots) {
        kfree(ckpt_cap_group->slots);
    }

    ckpt_cap_group->slots = (struct ckpt_object_slot *)kmalloc(
            (ckpt_cap_group->table_size) * sizeof(struct ckpt_object_slot));

    count = 0;
    struct ckpt_object *new_ckpt_obj;
    struct ckpt_obj_root *new_obj_root;
    u64 slot_id;
    for (i = 0; i < bmp_long_count; i++) {
        /* Skip 1st cap (cap group itself)*/
        map = i == 0 ? slot_table->slots_bmp[i] >> 1 : slot_table->slots_bmp[i];
        offset = i == 0 ? 1 : 0;
        /* Traverse these 64 slots */
        for (; map; offset++, map >>= 1) {
            if (map & 1) {
                /* The bit is present */
                slot_id = i * BITS_PER_LONG + offset;
                new_obj_root = ckpt_obj_root_get(
                        slot_table->slots[slot_id]->object, true);
                BUG_ON(!new_obj_root);
#ifdef REPORT
                stop();
#endif
                new_ckpt_obj = ckpt_obj_get(new_obj_root, true);
#ifdef REPORT
                start();
#endif

                if (!new_ckpt_obj) {
                    r = -ENOMEM;
                    kwarn("ckpt_obj_get_error\n");
                    goto out_free_prev_obj;
                }
                /* Store slot */
                BUG_ON(count >= ckpt_cap_group->table_size);
                ckpt_cap_group->slots[count].slot_id = slot_id;
                ckpt_cap_group->slots[count].obj_root = new_obj_root;
                count++;
            }
        }
    }
#ifdef REPORT
    eval_obj_time[TYPE_CAP_GROUP] += stop();
#endif

    return 0;
/* TODO: if alloc fails, we should free all created objs */
out_free_prev_obj:
    return r;
}

/*
 * cap_ckpt only copy pmo object,
 * but cap_group_ckpt should copy all objects
 */
int cap_group_ckpt(struct cap_group *cap_group,
                        struct ckpt_cap_group *ckpt_cap_group)
{
    kdebug("ckpt cap group %lx: size %u, badge %lx, name %s\n",
           cap_group,
           cap_group->slot_table.slots_size,
           cap_group->badge,
           cap_group->cap_group_name);
    /* Copy properties */
    ckpt_cap_group->badge = cap_group->badge;
    ckpt_cap_group->notify_recycler = cap_group->notify_recycler;
    if (ckpt_cap_group->cap_group_name[0] == 0)
        memcpy(ckpt_cap_group->cap_group_name,
               cap_group->cap_group_name,
               MAX_GROUP_NAME_LEN);

    /* Copy slot table */
    return slot_table_ckpt(cap_group, ckpt_cap_group);
}

int slot_table_restore(struct cap_group *cap_group,
                       struct ckpt_cap_group *ckpt_cap_group,
                       struct kvs *obj_map)
{
    struct ckpt_obj_root *ckpt_obj_root;
    struct object *new_obj;
    u64 slot_id;
    int ckpt_table_size;
    int r, i, cap;

#ifdef RESTORE_REPORT
    DECLTMR;
    start();
#endif

    /* put the cap of the cap_group its self on the first slot */
    cap = cap_insert(cap_group, cap_group, 0, CAP_GROUP_OBJ_ID);
    if (cap < 0) {
        r = cap;
        BUG("insert cap error\n");
        goto out_fail;
    }

    /* restore slot table, skip 1st cap */
    ckpt_table_size = ckpt_cap_group->table_size;
    for (i = 0; i < ckpt_table_size; i++) {
        slot_id = ckpt_cap_group->slots[i].slot_id;
        ckpt_obj_root = ckpt_cap_group->slots[i].obj_root;
#ifdef RESTORE_REPORT
        stop();
#endif
        new_obj = restore_obj_get_by_cap_group(ckpt_obj_root, obj_map, FLAGS_ALLOC);
#ifdef RESTORE_REPORT
        start();
#endif
        if (!new_obj) {
            r = -ENOMEM;
            goto out_free_prev_obj;
        }
        cap = cap_insert(cap_group, new_obj->opaque, 0, slot_id);
        if (cap < 0) {
            r = cap;
            BUG("insert cap error\n");
            goto out_free_prev_obj;
        }
    }
#ifdef RESTORE_REPORT
    eval_restore_obj_time[TYPE_CAP_GROUP] += stop();
#endif
    kdebug("cap group: %lx, vm: %lx\n",
           cap_group,
           obj_get(cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE));
    return 0;
out_free_prev_obj:
out_fail:
    return r;
}

extern int cap_group_init(struct cap_group *cap_group, unsigned int size,
                          u64 badge);
int cap_group_restore(struct object *cap_group_obj,
                      struct ckpt_object *ckpt_cap_group_obj,
                      struct kvs *obj_map)
{
    struct cap_group *cap_group = (struct cap_group *)cap_group_obj->opaque;
    struct ckpt_cap_group *ckpt_cap_group =
            (struct ckpt_cap_group *)ckpt_cap_group_obj->opaque;

    kdebug("restore cap group%lx: size %u, badge %lx, name %s\n",
           cap_group,
           ckpt_cap_group->slots_size,
           ckpt_cap_group->badge,
           ckpt_cap_group->cap_group_name);

    /* cap_group init */
    cap_group_init(
            cap_group, ckpt_cap_group->slots_size, ckpt_cap_group->badge);
    memcpy(cap_group->cap_group_name,
           ckpt_cap_group->cap_group_name,
           MAX_GROUP_NAME_LEN);
    cap_group->notify_recycler = ckpt_cap_group->notify_recycler;

    /* Restore slot table */
    return slot_table_restore(cap_group, ckpt_cap_group, obj_map);
}
