#pragma once

#include <ckpt/ckpt_data.h>

struct ckpt_ws_data* get_ckpt_ws_data();

int ckpt_obj_map_init(void);

struct ckpt_object *ckpt_obj_alloc(u64 type);

struct ckpt_object *ckpt_obj_get(struct ckpt_obj_root *obj_root, int alloc);

struct object *restore_obj_get(struct ckpt_obj_root *ckpt_obj_root);

struct object *restore_obj_get_by_cap_group(struct ckpt_obj_root *ckpt_obj_root, struct kvs *obj_map,int alloc);

struct ckpt_obj_root *ckpt_obj_root_alloc();

struct ckpt_obj_root *ckpt_obj_root_get(struct object *obj, int alloc);

struct ckpt_obj_root *get_copied_obj_root(struct ckpt_obj_root *ckpt_obj_root, struct kvs *obj_map);

struct ckpt_object *get_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root, u64 version_number);

struct ckpt_object *get_second_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root, u64 version_number);

void set_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root, u64 version_number, struct ckpt_object *ckpt_obj);

void set_second_latest_ckpt_obj(struct ckpt_obj_root *ckpt_obj_root, u64 version_number, struct ckpt_object *ckpt_obj);

void dsm_enqueue(void *data);

void *dsm_dequeue();
