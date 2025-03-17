#ifdef CHCORE_ENABLE_FMAP

/**
 * User-level lib for handling user page fault in fmap
 */
#include <time.h>
#include <pthread.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <chcore-internal/fs_debug.h>
#include <chcore/defs.h>
#include <chcore/ring_buffer.h>

#include "fs_page_fault.h"
#include "fs_page_cache.h"
#include "fs_wrapper_defs.h"

struct ring_buffer *fault_msg_buffer;
#define MAX_MSG_NUM 100
int notific_cap;
struct list_head fmap_area_mappings;
pthread_rwlock_t fmap_area_lock;

/**
 * If page cache module is available,
 *      use addr of page cache page first.
 * Else,
 *      use specific operation defined by under file system (eg. tmpfs)
 * Return (vaddr_t)0 as error.
 */
vaddr_t fs_wrapper_fmap_get_page_addr(struct fs_vnode *vnode, size_t offset)
{
	vaddr_t page_buf;
	int page_idx;

	pthread_rwlock_rdlock(&vnode->rwlock);

	assert(offset % PAGE_SIZE == 0);
	if (offset >= ROUND_UP(vnode->size, PAGE_SIZE)) {
		/* out-of-range */
		pthread_rwlock_unlock(&vnode->rwlock);
		return (vaddr_t)0;
	}

	if (using_page_cache) {
		page_idx = offset / PAGE_SIZE;
		page_buf = (vaddr_t)page_cache_get_block_or_page(
			vnode->page_cache, page_idx, -1, READ);
	} else {
		page_buf =
			server_ops.fmap_get_page_addr(vnode->private, offset);
	}

	pthread_rwlock_unlock(&vnode->rwlock);
	return (vaddr_t)page_buf;
}

static int handle_one_fault(u64 fault_badge, vaddr_t fault_va)
{
	vaddr_t server_page_addr;
	size_t area_off;
	struct fs_vnode *vnode;
	size_t file_offset;
	u64 flags;
	bool copy = 0;
	int ret;

	fs_debug_trace_fswrapper(
		"badge=0x%lx, va=0x%lx\n", fault_badge, fault_va);

	/* Find mapping area info */
	ret = fmap_area_find(
		fault_badge, fault_va, &area_off, &vnode, &file_offset, &flags);
		
	// TODO(yjs) handle error here (handle_one_fault to wrong machine)

	if (ret < 0) {
		/* TODO: handle errors */
		fs_debug_error("ret = %d\n", ret);
		BUG_ON("why a fault happened when not recorded\n");
	}

	fs_debug_trace_fswrapper(
		"fmap_area: area_off=0x%lx, file_off=0x%lx, flags=%ld\n",
		area_off,
		file_offset,
		flags);

	/* Get a server address space page va for mapping client */
	server_page_addr =
		fs_wrapper_fmap_get_page_addr(vnode, file_offset + area_off);
	if (!server_page_addr) {
		/* The file offset is out-of-range */
		fs_debug_warn("vnode->size=0x%lx, offset=0x%lx\n",
			      vnode->size,
			      file_offset + area_off);
	}

	/* Handle flags */
	BUG_ON(!(flags & MAP_SHARED || flags & MAP_PRIVATE));
	if (flags & MAP_SHARED) {
		copy = 0;
		if (!server_page_addr) {
			/* The file offset is out-of-range */
			fs_debug_warn("vnode->size=0x%lx, offset=0x%lx\n",
				      vnode->size,
				      file_offset + area_off);

			pthread_rwlock_wrlock(&vnode->rwlock);
			ret = server_ops.ftruncate(vnode->private,
						   file_offset + area_off
							   + PAGE_SIZE);
			pthread_rwlock_unlock(&vnode->rwlock);
			BUG_ON(ret); /* TODO */
			vnode->size = file_offset + area_off + PAGE_SIZE;
			server_page_addr = fs_wrapper_fmap_get_page_addr(
				vnode, file_offset + area_off);
			BUG_ON(!server_page_addr); /* TODO */
		}
	} else if (flags & MAP_PRIVATE) {
		copy = 1;
		if (!server_page_addr) {
			/* The file offset is out-of-range */
			fs_debug_warn("vnode->size=0x%lx, offset=0x%lx\n",
				      vnode->size,
				      file_offset + area_off);

			/**
			 * NOTE:
			 * When remap_va is 0,
			 *      the syscall will create a blank page for client.
			 * Don't do anything to change the data in server side.
			 */
			}
	}

	/* Map client page table, and notify fault thread */
	ret = usys_user_fault_map(
		fault_badge, fault_va, server_page_addr, copy);
	if (ret < 0) {
		/* TODO: handle errors */
		BUG_ON("this call should always be success here\n");
	}

	return 0;
}

void *user_fault_handler(void *args)
{
	struct user_fault_msg msg;
	int ret;

	while (1) {
		usys_wait(notific_cap, 1 /* Block */, NULL);
		while (get_one_msg(fault_msg_buffer, &msg)) {
			fs_debug_trace_fswrapper(
				"fault_msg_slot: 0x%lx\n",
				(vaddr_t)fault_msg_buffer);
			/* Handle msg */
			ret = handle_one_fault(msg.fault_badge, msg.fault_va);
			if (ret) {
				fs_debug_error("ret = %d \n", ret);
			}
		}
	}
	return NULL;
}

int fs_page_fault_init()
{
	int ret;
	pthread_t fh;

	/* Create a ring buffer to recieve kernel fault msg */
	/* FIXME: why a not aligned va will trigger error? */
	fault_msg_buffer = new_ringbuffer(MAX_MSG_NUM, sizeof(struct user_fault_msg));
	if (fault_msg_buffer == 0)
		return -ENOMEM;

	/* Create a notification for fault handler */
	notific_cap = usys_create_notifc();
	if (notific_cap < 0)
		return notific_cap;

	/* Register fmap_fault_pool in kernel using syscall */
	ret = usys_user_fault_register(notific_cap, (vaddr_t)fault_msg_buffer);
	if (ret < 0) {
		free_ringbuffer(fault_msg_buffer);
		return ret;
	}

	/* Init fmap_area_mapping list */
	init_list_head(&fmap_area_mappings);
	pthread_rwlock_init(&fmap_area_lock, NULL);

	/* Create fault handler to do user-level page fault */
	ret = pthread_create(&fh, NULL, user_fault_handler, NULL);
	if (ret < 0) {
		free_ringbuffer(fault_msg_buffer);
		return ret;
	}

	return 0;
}

/**
 * Helpers for fmap_area_mappings
 */

int fmap_area_insert(u64 client_badge, vaddr_t client_va_start, size_t length,
		     struct fs_vnode *vnode, size_t file_offset, u64 flags)
{
	struct fmap_area_mapping *mapping;

	mapping = (struct fmap_area_mapping *)malloc(sizeof(*mapping));
	if (!mapping)
		return -ENOMEM;

	mapping->client_badge = client_badge;
	mapping->client_va_start = client_va_start;
	mapping->length = length;
	mapping->vnode = vnode;
	inc_ref_fs_vnode(vnode);
	mapping->file_offset = file_offset;
	mapping->flags = flags;

	fs_debug_trace_fswrapper(
		"client_badge=0x%lx, client_va_start=0x%lx, length=%ld\n"
		 "vnode->id=%ld, file_offset=%ld, flags=%ld\n",
		 client_badge,
		 client_va_start,
		 length,
		 vnode->vnode_id,
		 file_offset,
		 flags);
	pthread_rwlock_wrlock(&fmap_area_lock);
	list_append(&mapping->node, &fmap_area_mappings);
	pthread_rwlock_unlock(&fmap_area_lock);
	return 0;
}

/**
 * [IN] client_badge, client_va
 * [OUT] area_off, vnode, file_offset
 */
int fmap_area_find(u64 client_badge, vaddr_t client_va, size_t *area_off,
		   struct fs_vnode **vnode, size_t *file_offset, u64 *flags)
{
	struct fmap_area_mapping *area_iter;
	pthread_rwlock_rdlock(&fmap_area_lock);
	for_each_in_list (area_iter,
			  struct fmap_area_mapping,
			  node,
			  &fmap_area_mappings) {
		if (area_iter->client_badge == client_badge
		    && (area_iter->client_va_start <= client_va)
		    && (area_iter->client_va_start + area_iter->length
			> client_va)) {
			/* Hit */
			*area_off = client_va - area_iter->client_va_start;
			*vnode = area_iter->vnode;
			*file_offset = area_iter->file_offset;
			*flags = area_iter->flags;
			pthread_rwlock_unlock(&fmap_area_lock);
			return 0;
		}
	}
	pthread_rwlock_unlock(&fmap_area_lock);

	return -1; /* Not Found */
}

#endif
