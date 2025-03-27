#pragma once

#include <object/object.h>
#include <dsm/tiering.h>

/* Copy policy */
typedef int (*dsm_copy_func)(struct object *src_obj, struct object *dst_obj);

int dsm_copy_cap_group(struct object *src_obj, struct object *dst_obj);
int dsm_copy_thread(struct object *src, struct object *dst);
int dsm_copy_vmspace(struct object *src_obj, struct object *dst_obj);
int dsm_copy_connection(struct object *src_obj, struct object *dst_obj);
int dsm_copy_notification(struct object *src_obj, struct object *dst_obj);
int dsm_copy_irq(struct object *src_obj, struct object *dst_obj);
int dsm_copy_pmo(struct object *src_obj, struct object *dst_obj);
