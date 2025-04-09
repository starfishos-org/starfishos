#include <common/errno.h>
#include <common/list.h>
#include <common/util.h>
#include <ckpt/ckpt-dsm.h>
#include <mm/uaccess.h>
#include <ckpt/ckpt-dsm.h>

#include "ckpt_ws.h"

static inline u64 __name_hash(char *name, u64 name_len)
{
    char c;
    u64 hash_val = 0;

    for (int i = 0; i < name_len; i++) {
        c = name[i];
        hash_val = hash_val * 31 + (c - 'a' + 1);
    }

    return hash_val;
}

/* init */
int ckpt_ws_init(void)
{
    int ret = 0;

    if (CKPT_WS_TABLE) {
        /* name_kvs and ws_list can be null */
        return 0;
    }

    /* Create ckpt whole-sys table */
    CKPT_WS_TABLE = (struct ckpt_ws_table *)kmalloc(
            sizeof(struct ckpt_ws_table), __MT_SHARED__);
    if (!CKPT_WS_TABLE) {
        kinfo("[CKPT WS] can not alloc ckpt_ws_table\n");
        ret = -ENOMEM;
        goto out_fail;
    }

    /* Create info->data kvs */
    CKPT_WS_TABLE->ckpt_ws_kvs = new_kvs(KVS_SIZE, __MT_SHARED__);
    if (!CKPT_WS_TABLE->ckpt_ws_kvs) {
        kinfo("[CKPT WS] can not alloc ckpt_ws_kvs\n");
        ret = -ENOMEM;
        goto out_fail;
    }

    /* Create name kvs */
    CKPT_WS_TABLE->name_kvs = new_kvs(KVS_SIZE, __MT_SHARED__);
    if (!CKPT_WS_TABLE->name_kvs) {
        kinfo("[CKPT WS] can not alloc name_kvs\n");
        ret = -ENOMEM;
        goto out_fail;
    }

    /* Create ckpt_ws_list */
    init_list_head(&(CKPT_WS_TABLE->ckpt_ws_list));

    set_current_ckpt_version(0);
    return 0;

out_fail:
    CKPT_WS_TABLE = NULL;
    return ret;
}

static inline u64 __query_info_list(struct list_head *info_list, char *name,
                                    u64 name_len)
{
    struct ckpt_ws_info *info;

    /* check if there are info name by *name* */
    for_each_in_list (info, struct ckpt_ws_info, kvs_val_node, info_list) {
        if (info->name_len != name_len)
            continue;
        if (!strncmp((char *)info->name, name, name_len))
            return (u64)info;
    }

    return 0;
}

int __name_kvs_put(struct ckpt_ws_info *info)
{
    int ret = 0;
    u64 hash_val;
    ckpt_ws_info_list_t **info_list_head_val;
    ckpt_ws_info_list_t *info_list_head;

    hash_val = __name_hash((char *)&(info->name), info->name_len);
    info_list_head_val = (ckpt_ws_info_list_t **)kvs_get(
            CKPT_WS_TABLE->ckpt_ws_kvs, (kvs_key_t *)&hash_val);

    if (!info_list_head_val) {
        info_list_head = (ckpt_ws_info_list_t *)kmalloc(sizeof(*info_list_head),
                                                        __MT_SHARED__);
        if (!info_list_head) {
            kinfo("[CKPT WS] fail to malloc info list\n");
            return -ENOMEM;
        }
        init_list_head(&info_list_head->list);
        list_add(&(info->kvs_val_node), &(info_list_head->list));
        ret = kvs_put(CKPT_WS_TABLE->name_kvs,
                      (kvs_key_t *)&hash_val,
                      (kvs_value_t *)&info_list_head);
    } else {
        /* check if the name is already used */
        info_list_head = *info_list_head_val;
        if (__query_info_list(&(info_list_head->list),
                              (char *)&(info->name),
                              info->name_len)) {
            kinfo("[CKPT WS] already used name(%s)\n", info->name);
            return -EINVAL;
        }
        list_add(&(info->kvs_val_node), &(info_list_head->list));
    }

    return ret;
}

/* get by id */
struct ckpt_ws_data *ckpt_ws_get(u64 ckpt_id)
{
    struct ckpt_ws_data **data;

    if (!CKPT_WS_TABLE || !CKPT_WS_TABLE->ckpt_ws_kvs) {
        kinfo("[CKPT WS] no global struct found\n");
        return NULL;
    }

    data = (struct ckpt_ws_data **)kvs_get(CKPT_WS_TABLE->ckpt_ws_kvs,
                                           (kvs_key_t *)&ckpt_id);
    if (!data) {
        kinfo("[CKPT WS] can not find obj(id=%lu)", ckpt_id);
        return NULL;
    }

    return *data;
}

/* get latest ckpt */
struct ckpt_ws_data *ckpt_ws_get_latest()
{
    struct ckpt_ws_info *latest;

    if (!CKPT_WS_TABLE || !&(CKPT_WS_TABLE->ckpt_ws_list)) {
        kinfo("[CKPT WS] no global struct found\n");
        return NULL;
    }

    if (list_empty(&CKPT_WS_TABLE->ckpt_ws_list)) {
        kinfo("[CKPT WS] no old ckpt found\n");
        return NULL;
    }

    latest = list_entry(
            CKPT_WS_TABLE->ckpt_ws_list.next, struct ckpt_ws_info, node);
    kinfo("[CKPT WS] latest ckpt(id=%lx) found\n", (u64)latest);
    return ckpt_ws_get((u64)latest);
}

/* put ckpt data */
u64 ckpt_ws_put(struct ckpt_ws_data *ckpt_data, char *name, u64 name_len)
{
    struct ckpt_ws_info *info;
    int ret = 0;

    info = (struct ckpt_ws_info *)kmalloc(sizeof(*info), __MT_SHARED__);
    if (!info) {
        kinfo("[CKPT WS] can not allocate memory for ckpt info.\n");
        return 0;
    }

    info->ckpt_data = ckpt_data;
    info->ts = (timestamp_t)plat_get_mono_time();
    if (name_len) {
        if (name_len > MAX_CKPT_NAME_LEN) {
            name_len = MAX_CKPT_NAME_LEN;
        }
        memcpy(info->name, name, name_len);
    }
    info->name_len = name_len;

    /* add pair(info, data) */
    kvs_put(CKPT_WS_TABLE->ckpt_ws_kvs,
            (kvs_key_t *)&info,
            (kvs_value_t *)&ckpt_data);

    /* add pair(hash(name), info*) to kvs */
    ret = __name_kvs_put(info);
    if (ret) {
        kinfo("[CKPT WS] error during add name kvs\n");
        return 0;
    }

    /* add to list is atomic point */
    list_add(&(info->node), &(CKPT_WS_TABLE->ckpt_ws_list));

    return (u64)info;
}

u64 ckpt_ws_put_from_userspace(struct ckpt_ws_data *ckpt_data, u64 name_buf,
                               u64 name_len)
{
    struct ckpt_ws_info *info;
    int ret = 0;

    info = (struct ckpt_ws_info *)kmalloc(sizeof(*info), __MT_SHARED__);
    if (!info) {
        kinfo("[CKPT WS] can not allocate memory for ckpt info.\n");
        return 0;
    }

    info->ckpt_data = ckpt_data;
    info->ts = (timestamp_t)plat_get_mono_time();

    if (name_buf) {
        /* user specify name */
        if (name_len > MAX_CKPT_NAME_LEN) {
            kinfo("[CKPT WS] only support name len < %d, truncked\n",
                  MAX_CKPT_NAME_LEN);
            info->name_len = MAX_CKPT_NAME_LEN;
        } else
            info->name_len = name_len;

        ret = copy_from_user(
                (char *)(info->name), (char *)name_buf, info->name_len);
        if (ret) {
            kinfo("[CKPT WS] error during copy from user\n");
            return 0;
        }
    } else {
        info->name_len = 0;
    }

    /* add pair(info, data) */
    kvs_put(CKPT_WS_TABLE->ckpt_ws_kvs,
            (kvs_key_t *)&info,
            (kvs_value_t *)&ckpt_data);

    /* add pair(hash(name), info*) to kvs */
    ret = __name_kvs_put(info);
    if (ret) {
        kinfo("[CKPT WS] error during add name kvs\n");
        return 0;
    }

    /* add to list is atomic point */
    list_add(&(info->node), &(CKPT_WS_TABLE->ckpt_ws_list));

    return (u64)info;
}

/*
 * return ckpt_id if found, else return 0
 */
struct ckpt_ws_info *ckpt_ws_query_by_name(char *name, u64 name_len)
{
    ckpt_ws_info_list_t *info_list_head;
    ckpt_ws_info_list_t **info_list_head_ptr;
    u64 hash_val, ckpt_id;

    BUG_ON(!name || name_len == 0);
    BUG_ON(!CKPT_WS_TABLE || !CKPT_WS_TABLE->name_kvs);

    /* get info list */
    hash_val = __name_hash(name, name_len);
    info_list_head_ptr = ((ckpt_ws_info_list_t **)kvs_get(
            CKPT_WS_TABLE->name_kvs, (kvs_key_t *)&hash_val));
    if (!info_list_head_ptr) {
        goto not_found;
    }

    info_list_head = *info_list_head_ptr;
    ckpt_id = __query_info_list(&(info_list_head->list), name, name_len);
    if (!ckpt_id) {
        kinfo("[CKPT WS] no ckpt found with name %s\n", name);
        goto not_found;
    }

    return (struct ckpt_ws_info *)ckpt_id;
not_found:
    // kinfo("[CKPT WS] no ckpt found with name %s\n", name);
    return NULL;
}