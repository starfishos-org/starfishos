#include <object/cap_group.h>

#include "../dsm_tiering.h"

int dsm_copy_cap_group(struct object *src_obj, struct object *dst_obj)
{
    struct cap_group *src_cap_group = (struct cap_group *)src_obj->opaque;
    struct cap_group *dst_cap_group = (struct cap_group *)dst_obj->opaque;

    /* Copy basic fields */
    dst_cap_group->badge = src_cap_group->badge;
    dst_cap_group->notify_recycler = src_cap_group->notify_recycler;
    memcpy(dst_cap_group->cap_group_name, src_cap_group->cap_group_name, MAX_GROUP_NAME_LEN);

    /* Copy slot table */
    dst_cap_group->slot_table.slots_size = src_cap_group->slot_table.slots_size;
    dst_cap_group->slot_table.slots_bmp = kmalloc(sizeof(unsigned long) * BITS_TO_LONGS(src_cap_group->slot_table.slots_size), __MT_SHARED__);
    if (!dst_cap_group->slot_table.slots_bmp) {
        return -ENOMEM;
    }
    memcpy(dst_cap_group->slot_table.slots_bmp, src_cap_group->slot_table.slots_bmp, 
           sizeof(unsigned long) * BITS_TO_LONGS(src_cap_group->slot_table.slots_size));

    /* Copy slots */
    dst_cap_group->slot_table.slots = kmalloc(sizeof(struct object *) * src_cap_group->slot_table.slots_size, __MT_SHARED__);
    if (!dst_cap_group->slot_table.slots) {
        kfree(dst_cap_group->slot_table.slots_bmp);
        return -ENOMEM;
    }
    memcpy(dst_cap_group->slot_table.slots, src_cap_group->slot_table.slots,
           sizeof(struct object *) * src_cap_group->slot_table.slots_size);

    return 0;
}
