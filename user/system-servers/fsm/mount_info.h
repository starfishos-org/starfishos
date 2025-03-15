#pragma once

#include <chcore-internal/fs_defs.h>
#include <chcore/container/list.h>
#include <string.h>
#include <malloc.h>
#include "defs.h"

/* ------------------------------------------------------------------------ */

/*
 * Mount Point Informations
 */
struct mount_point_info_node {
	int fs_cap;
	int target_machine_id;
	char path[MAX_MOUNT_POINT_LEN + 1];
	int path_len;
	ipc_struct_t *_fs_ipc_struct;
	int refcnt;

	struct list_head node;
};

extern int fs_num;

extern struct list_head mount_point_infos;
extern pthread_rwlock_t mount_point_infos_rwlock;

struct mount_point_info_node *set_mount_point(const char *path, int path_len, int fs_cap, int target_machine_id);
struct mount_point_info_node *get_mount_point(char *path, int path_len);
int remove_mount_point(char *path);
