#include <object/cap_group.h>

#include "../dsm_tiering.h"

extern int slot_table_init(struct slot_table *slot_table, unsigned int size,
                           bool init_lock, mem_t mem_type);
extern int slot_table_free(struct slot_table *slot_table);

int dsm_copy_slot_table(struct cap_group *src_cap_group, struct cap_group *dst_cap_group, mem_t mem_type)
{
    struct slot_table *src_slot_table = &src_cap_group->slot_table;
    struct slot_table *dst_slot_table = &dst_cap_group->slot_table;
    int src_slot_size = src_slot_table->slots_size;

    /* Allocate new slots_bmp and slots if the size is different */
    if (dst_slot_table->slots_size != src_slot_size) {
        slot_table_free(dst_slot_table);
        slot_table_init(dst_slot_table, src_slot_size, true, mem_type);
    }

    // NOTE: can not skip as number is the same but data can be different
    // memcpy(dst_slot_table->slots_bmp, src_slot_table->slots_bmp, 
    //        sizeof(unsigned long) * BITS_TO_LONGS(src_slot_table->slots_size));

    /* Copy slots */
    // NOTE: should use new object slot to accelerate object access
    int slot_id;
    struct object *dst_object;
    struct object_slot *src_slot;
    
    for_each_set_bit (slot_id, src_slot_table->slots_bmp, src_slot_size) {
        src_slot = src_slot_table->slots[slot_id];
        BUG_ON(src_slot == NULL);

        /* 1st cap is cap_group */
        if (slot_id == CAP_GROUP_OBJ_ID) {
            dst_object = obj2object(dst_cap_group);
        } else {
            /* 
            * @NOTE: demote cap group will not demote each object
            *        instead, we demote each object before manually
            * But, we will create a shared object at here.
            */
            dst_object = dsm_get_object_by_mem_type(
                            src_slot->object, mem_type, true);
            BUG_ON(dst_object == NULL);
        }

        /* insert cap */
        cap_insert(dst_cap_group, dst_object, src_slot->rights, slot_id, mem_type);
        // BUG_ON(!dst_slot_table->slots[slot_id]);
        // BUG_ON(!get_bit(slot_id, dst_slot_table->slots_bmp));

        DSM_TIER_LOG_DEBUG("[table=%p] install slot: slot_id: %d, object: %p, type: %s\n", 
            dst_slot_table, slot_id, dst_object, obj_name_tbl[dst_object->type]);
    }

    return 0;
}

int dsm_copy_cap_group(struct object *src_obj, struct object *dst_obj)
{
    struct cap_group *src_cap_group = (struct cap_group *)src_obj->opaque;
    struct cap_group *dst_cap_group = (struct cap_group *)dst_obj->opaque;
    int is_demote = is_private_object(src_obj);

    // DSM_TIER_LOG_DEBUG("src_cg: %p, src_cg_object: %p\n", 
    //     src_cap_group, src_obj);

    /* Copy basic info */
    dst_cap_group->badge = src_cap_group->badge;
    memcpy(dst_cap_group->cap_group_name, src_cap_group->cap_group_name, 
           MAX_GROUP_NAME_LEN);
    init_list_head(&dst_cap_group->thread_list);
    lock_init(&dst_cap_group->threads_lock);
    /* thread_cnt will wait for the thread to be inserted */
    dst_cap_group->thread_cnt = 0;
    /* TODO: notify_recycler should be set to new recycler */
    dst_cap_group->notify_recycler = 0;

    /* Copy slots */
    dsm_copy_slot_table(src_cap_group, dst_cap_group, 
                        is_demote ? __MT_SHARED__ : __MT_PRIVATE__);

    return 0;
}
