#ifndef __CKPT_CFORK_H__
#define __CKPT_CFORK_H__

#include <common/types.h>
#include <object/cap_group.h>
#include <common/list.h>
#include <ckpt/ckpt.h>

#include "log.h"

// Helper functions
// cap_kvs
struct cap_group *find_capgroup_by_name(char *pname, u64 pname_len);
struct ckpt_obj_root *find_ckpt_obj_root_by_name(char *pname, u64 pname_len);
int add_ckpt_obj_root_by_name(struct ckpt_obj_root *ckpt_obj_root, char *pname, u64 pname_len);

// TODO: these are general functions, move them to a more general place
// int stop_all_threads(struct list_head *thread_list);
// int start_all_threads(struct list_head *thread_list);

// cfork prepare
struct ckpt_obj_root *cfork_prepare_ckpt_process(struct object *root_cg_obj);
int cfork_ckpt_process(struct ckpt_obj_root *ckpt_obj_root);
int cfork_restore_process(struct ckpt_obj_root *ckpt_obj_root, struct cap_group **out_root_cg);

int add_cap_group_to_cap_tree(struct cap_group *root_cap_group, struct cap_group *restored_cg);

// int dsm_migrate_process_prepare(struct object *root_cg_obj);

#endif // __CKPT_CFORK_H__
