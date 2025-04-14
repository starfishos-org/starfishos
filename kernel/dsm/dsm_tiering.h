#pragma once

#include <object/object.h>
#include <dsm/tiering.h>

#include "log.h"

/* Copy policy */
typedef int (*dsm_copy_func)(struct object *src_obj, struct object *dst_obj);
typedef int (*dsm_stw_copy_func)(struct object *src_obj, struct object *dst_obj);

int dsm_copy_cap_group(struct object *src_obj, struct object *dst_obj);
int dsm_copy_thread(struct object *src, struct object *dst);
int dsm_copy_vmspace(struct object *src_obj, struct object *dst_obj);
int dsm_copy_connection(struct object *src_obj, struct object *dst_obj);
int dsm_copy_notification(struct object *src_obj, struct object *dst_obj);
int dsm_copy_irq(struct object *src_obj, struct object *dst_obj);
int dsm_copy_pmo(struct object *src_obj, struct object *dst_obj);

int dsm_stw_copy_cap_group(struct object *src_obj, struct object *dst_obj);
int dsm_stw_copy_thread(struct object *src_obj, struct object *dst_obj);
int dsm_stw_copy_connection(struct object *src_obj, struct object *dst_obj);
int dsm_stw_copy_notification(struct object *src_obj, struct object *dst_obj);
int dsm_stw_copy_irq(struct object *src_obj, struct object *dst_obj);
int dsm_stw_copy_vmspace(struct object *src_obj, struct object *dst_obj);
int dsm_stw_copy_pmo(struct object *src_obj, struct object *dst_obj);

/* helper functions */
int dsm_copy_page_table(struct object *src_obj, struct object *dst_obj);
int add_thread_to_cap_group(struct thread *dst_thread, struct cap_group *src_cap_group);

int demote_each_object_in_cap_group(struct cap_group *cap_group, u64 type_mask);
int promote_each_object_in_cap_group(struct cap_group *cap_group, u64 type_mask);
int stw_copy_each_object_in_cap_group(struct cap_group *cap_group, u64 type_mask);
