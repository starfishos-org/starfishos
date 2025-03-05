#include <object/thread.h>
#include <ckpt/ckpt_data.h>
#include <common/kvstore.h>
#include <common/util.h>
#include <arch/mmu.h>
#include <object/cap_group.h>
#include <object/user_fault.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/kmalloc.h>
#include <ckpt/ckpt.h>
#include <sched/context.h>
#include <perf/measure.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/hybird_mem.h>
#include <ckpt/external_sync.h>
#include <common/list.h>

#include "ckpt_ws.h"
#include "ckpt_object_pool.h"
#include "ckpt_objects.h"
#include "log.h"

void flush_tlb_all(void);


struct found_cg {
    struct cap_group *cap_group;
    struct list_head list_node;
};

/**
 * Find the cap group in the cap tree by the process name.
 * @param cap_group: the root of the cap tree (a subtree of the cap tree).
 * @param pname: the process name.
 * @return: the cap group if found, NULL otherwise.
 */
static struct cap_group *
find_cap_group_in_cap_tree(struct cap_group *start_cap_group, char *pname)
{
    struct slot_table *slot_table;
    struct object *object;
    struct cap_group *cap_group;
    char *cap_group_name;
    u64 map, offset, slot_id;
    int bmp_long_count, i;
    struct list_head _cap_group_list;
    struct found_cg *result, *tmp;

    init_list_head(&_cap_group_list);

    slot_table = &start_cap_group->slot_table;
    bmp_long_count = BITS_TO_LONGS(slot_table->slots_size);

    for (i = 0; i < bmp_long_count; i++) {
        /* Skip 1st cap (cap group itself)*/
        map = i == 0 ? slot_table->slots_bmp[i] >> 1 : slot_table->slots_bmp[i];
        offset = i == 0 ? 1 : 0;
        /* Traverse these 64 slots */
        for (; map; offset++, map >>= 1) {
            if (!(map & 1)) {
                continue;
            }

            /* The bit is present */
            slot_id = i * BITS_PER_LONG + offset;
            object = slot_table->slots[slot_id]->object;

            if (object->type != TYPE_CAP_GROUP) 
                continue;

            cap_group = (struct cap_group *)object->opaque;
            cap_group_name = cap_group->cap_group_name;

            // truncate the cap group name
            if (cap_group_name[0] == '/') {
                cap_group_name = cap_group_name + 1;
            }

            if (strcmp((void *)cap_group_name, pname) == 0) {
                return cap_group;
            } else {
                // record the cap group found in a list
                struct found_cg *result = (struct found_cg *)kmalloc(
                        sizeof(struct found_cg), __PRIVATE__);
                result->cap_group = cap_group;
                list_add(&result->list_node, &_cap_group_list);
            }
        }
    }

    // BFS all cap groups found before
    for_each_in_list_safe(result, tmp, list_node, &_cap_group_list) {
        cap_group = find_cap_group_in_cap_tree(result->cap_group, pname);
        if (cap_group) {
            return cap_group;
        }
    }

    // destroy the list
    for_each_in_list_safe(result, tmp, list_node, &_cap_group_list) {
        list_del(&result->list_node);
        kfree(result);
    }

    return NULL;
}

int sys_cfork_prepare(u64 pname_ptr, u64 pname_len)
{
    char *pname;
    int ret = 0;

    pname = (char *)kmalloc(pname_len, __PRIVATE__);
    copy_from_user(pname, (void *)pname_ptr, pname_len);
    CFORK_LOG_INFO("cfork_prepare: pname: %s, pname_len: %d", pname, pname_len);

    // find the cap group in the cap tree by the process name
    struct cap_group *cap_group = find_cap_group_in_cap_tree(
            (struct cap_group *)root_cap_group, pname);
    if (!cap_group) {
        CFORK_LOG_ERR("cfork_prepare: cap_group not found");
        ret = -ENOENT;
        goto out;
    }

    // move 

out:
    kfree(pname);
    return ret;
}

int sys_cfork_ckpt(u64 pname_ptr, u64 pname_len)
{
    char *pname;

    pname = (char *)kmalloc(pname_len, __PRIVATE__);
    copy_from_user(pname, (void *)pname_ptr, pname_len);
    CFORK_LOG_INFO("cfork_ckpt: pname: %s, pname_len: %d", pname, pname_len);

    // remove the process from the cap tree
    // chcore_remove_process(pname);

    kfree(pname);

    return 0;
}

int sys_cfork_restore(u64 pname_ptr, u64 pname_len)
{
    char *pname;

    pname = (char *)kmalloc(pname_len, __PRIVATE__);

    copy_from_user(pname, (void *)pname_ptr, pname_len);
    CFORK_LOG_INFO("cfork_restore: pname: %s, pname_len: %d", pname, pname_len);

    kfree(pname);

    return 0;
}
