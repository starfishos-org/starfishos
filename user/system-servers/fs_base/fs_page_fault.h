#pragma once

#include <errno.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include <chcore/ipc.h>
#include <chcore/proc.h>
#include <chcore/syscall.h>
#include <chcore/container/list.h>
#include "fs_vnode.h"

/* Same structure in kernel, item of user-level ring buffer */
struct user_fault_msg {
	u64 fault_badge;
	vaddr_t fault_va;
};

/* Mapping from client mmap area to server vnode structure */
struct fmap_area_mapping {
	u64 client_badge;
	vaddr_t client_va_start;
	size_t length;

	struct fs_vnode *vnode;
	size_t file_offset;
	u64 flags;

	struct list_head node;
};

extern struct list_head fmap_area_mappings;
extern pthread_rwlock_t fmap_area_lock;

int fs_page_fault_init();

int fmap_area_insert(u64 client_badge, vaddr_t client_va_start, size_t length,
		     struct fs_vnode *vnode, size_t file_offset, u64 flags);
int fmap_area_find(u64 client_badge, vaddr_t client_va, size_t *area_off,
		   struct fs_vnode **vnode, size_t *file_offset, u64 *flags);