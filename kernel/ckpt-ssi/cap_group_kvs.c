#include <common/kvstore.h>
#include <common/util.h>
#include <common/waitlist.h>
#include <mm/kmalloc.h>
#include <object/cap_group.h>
#include <object/thread.h>
#include <dsm/dsm-single.h>
#include <ckpt/ckpt-dsm.h>

#include "cfork.h"
#include "ckpt_ws.h"

struct pname_kvs_value {
    char pname[128];
    struct ckpt_obj_root *ckpt_obj_root;
};

// a kvstore that maps the cap group name to the cap group
struct kvs *cap_kvs;

static void init_cg_kvs(void)
{
    if (unlikely(!cap_kvs)) {
        cap_kvs = new_kvs(KVS_SIZE, __MT_PRIVATE__);
    }
}

static inline u64 __pname_hash(char *name, u64 name_len)
{
    char c;
    u64 hash_val = 0;

    for (int i = 0; i < name_len; i++) {
        c = name[i];
        hash_val = hash_val * 31 + (c - 'a' + 1);
    }

    return hash_val;
}

/**
 * Find the cap group in the cap tree by the process name.
 * @param cap_group: the root of the cap tree (a subtree of the cap tree).
 * @param pname: the process name.
 * @return: the cap group if found, NULL otherwise.
 */
struct cap_group *
find_capgroup_in_captree(struct cap_group *start_cap_group, char *pname)
{
    struct slot_table *slot_table;
    struct object *object;
    struct cap_group *cap_group = NULL;
    char *cap_group_name;
    u64 map, offset, slot_id;
    int bmp_long_count, i;
    struct list_head _cap_group_list;
    wait_node_t *result, *tmp;

    // first find in the kvstore
    init_cg_kvs();
    cap_group = (struct cap_group *)kvs_get(cap_kvs, (kvs_key_t *)&pname);
    if (cap_group) {
        printk("cap_group found in kvstore: %s\n", pname);
        goto find_cap_group;
    }

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
                goto find_cap_group;
            } else {
                // record the cap group found in a list
                add_to_waiting_list(&_cap_group_list, (void *)cap_group);
            }
        }
    }

    // BFS all cap groups found before
    for_each_in_waitlist_safe(result, tmp, &_cap_group_list) {
        cap_group = find_capgroup_in_captree(
                (struct cap_group *)result->data, pname);
        if (cap_group) {
            goto find_cap_group;
        }

        // can remove the node from the list
        remove_from_waiting_list(&_cap_group_list, result);
    }

find_cap_group:
    if (cap_group) {
        kvs_put(cap_kvs, (kvs_key_t *)&(cap_group->cap_group_name), (kvs_value_t *)&cap_group);
    }

    return cap_group;
}

struct cap_group *find_capgroup_by_name(char *pname, u64 pname_len)
{
    return find_capgroup_in_captree(root_cap_group, pname);
}

/**
 * Add the cap group to the checkpoint cap tree.
 * @param ckpt_obj_root: ckpt object root point to the cap group.
 * @return: 0 if success, -1 otherwise.
 */
int add_ckpt_obj_root_by_name(struct ckpt_obj_root *ckpt_obj_root, char *pname, u64 pname_len)
{
    struct pname_kvs_value *pname_kvs_value;
    int ret;
    u64 hash_val = __pname_hash(pname, pname_len);

    pname_kvs_value = kmalloc(sizeof(*pname_kvs_value), __MT_SHARED__);
    if (!pname_kvs_value) {
        CFORK_LOG_ERR("add_ckpt_capgroup_by_name: kmalloc failed");
        return -ENOMEM;
    }
    memcpy(pname_kvs_value->pname, pname, pname_len);
    pname_kvs_value->ckpt_obj_root = ckpt_obj_root;
    
    ret = kvs_put(CKPT_CG_KVS, (kvs_key_t *)&hash_val, (kvs_value_t *)&pname_kvs_value);
    if (ret) {
        CFORK_LOG_ERR("add_ckpt_capgroup_by_name: kvs_put failed");
        return ret;
    }

    return 0;
}

/**
 * Find the cap group in the checkpoint cap tree by the process name.
 * @param start_cap_group: the root of the checkpoint cap tree (a subtree of the checkpoint cap tree).
 * @param pname: the process name.
 * @return: the cap group if found, NULL otherwise.
 */
struct ckpt_obj_root *
find_ckpt_obj_root_by_name(char *pname, u64 pname_len)
{
    u64 hash_val = __pname_hash(pname, pname_len);
    struct pname_kvs_value **pname_kvs_value_ptr;
    struct pname_kvs_value *pname_kvs_value;

    pname_kvs_value_ptr = (struct pname_kvs_value **)
        (kvs_get(CKPT_CG_KVS, (kvs_key_t*)&hash_val));
    if (!pname_kvs_value_ptr) {
        return NULL;
    }
    pname_kvs_value = *pname_kvs_value_ptr;

    if (pname_kvs_value && strncmp(pname_kvs_value->pname, pname, pname_len) == 0) {
        return pname_kvs_value->ckpt_obj_root;
    }

    // TODO: get the root of the checkpoint cap tree
    // currently, we do not search the checkpoint cap tree
    return NULL;
}

