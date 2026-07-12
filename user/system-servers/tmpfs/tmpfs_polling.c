#define _GNU_SOURCE
#include "tmpfs_polling.h"
#include "tmpfs.h"

#include <chcore/syscall.h>
#include <chcore/memory.h>
#include <chcore-internal/fs_defs.h>
#include <fs_wrapper_defs.h>
#include <fs_vnode.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>

#include <polling.h>
#include <polling_req.h>

/* durable_dequeue (with deferred-free) is shared via polling_req.h */

/* ================================================================
 * Direct request handlers — call fs internal functions directly,
 * no IPC involved.
 * ================================================================ */

extern pthread_rwlock_t fs_wrapper_meta_rwlock;

/* Auto-assign client-side fd for polling clients */
static int next_polling_fd = 1;

static void handle_open_direct(struct dq_node *node)
{
	char path[FS_REQ_PATH_BUF_LEN];
	int flags = node->req.open.flags;
	int mode = node->req.open.mode;
	strncpy(path, node->req.open.path, FS_REQ_PATH_BUF_LEN);
	path[FS_REQ_PATH_BUF_LEN - 1] = '\0';

	int client_fd = next_polling_fd++;
	if (client_fd >= MAX_SERVER_ENTRY_PER_CLIENT) {
		node->resp.open.fd = -1;
		return;
	}

	struct fs_request fr;
	memset(&fr, 0, sizeof(fr));
	fr.req = FS_REQ_OPEN;
	fr.open.new_fd = client_fd;
	strncpy(fr.open.pathname, path, FS_REQ_PATH_BUF_LEN);
	fr.open.flags = flags;
	fr.open.mode = mode;

	pthread_rwlock_wrlock(&fs_wrapper_meta_rwlock);
	int ret = fs_wrapper_open(POLLING_CLIENT_BADGE, NULL, &fr);
	pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);

	/* fs_wrapper_open returns new_fd on success, negative on error */
	node->resp.open.fd = ret;
}

static void handle_read_direct(struct dq_node *node)
{
	int client_fd = node->req.read.fd;
	size_t count = node->req.read.count;

	int fid = fs_wrapper_get_server_entry(POLLING_CLIENT_BADGE, client_fd);
	if (fid < 0) {
		node->resp.read.count = -1;
		return;
	}

	if (count > POLLING_FS_READ_BUF_SIZE)
		count = POLLING_FS_READ_BUF_SIZE;

	pthread_rwlock_rdlock(&fs_wrapper_meta_rwlock);

	if (fid >= MAX_SERVER_ENTRY_NUM || server_entrys[fid] == NULL) {
		pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);
		node->resp.read.count = -1;
		return;
	}

	struct server_entry *entry = server_entrys[fid];
	pthread_mutex_lock(&entry->lock);
	pthread_rwlock_rdlock(&entry->vnode->rwlock);

	off_t offset = entry->offset;
	ssize_t ret = 0;

	if (offset < (off_t)entry->vnode->size) {
		if (offset + count > entry->vnode->size)
			count = entry->vnode->size - offset;
		ret = server_ops.read(entry->vnode->private, offset, count,
				      node->resp.read.buf);
		entry->offset += ret;
	}

	pthread_rwlock_unlock(&entry->vnode->rwlock);
	pthread_mutex_unlock(&entry->lock);
	pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);

	node->resp.read.count = ret;
}

static void handle_write_direct(struct dq_node *node)
{
	int client_fd = node->req.write.fd;
	size_t count = node->req.write.count;

	int fid = fs_wrapper_get_server_entry(POLLING_CLIENT_BADGE, client_fd);
	if (fid < 0) {
		node->resp.write.count = -1;
		return;
	}

	if (count > POLLING_FS_WRITE_BUF_SIZE)
		count = POLLING_FS_WRITE_BUF_SIZE;

	/* Copy write data before resp overwrites req (they share a union) */
	char buf[POLLING_FS_WRITE_BUF_SIZE];
	memcpy(buf, node->req.write.buf, count);

	pthread_rwlock_rdlock(&fs_wrapper_meta_rwlock);

	if (fid >= MAX_SERVER_ENTRY_NUM || server_entrys[fid] == NULL) {
		pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);
		node->resp.write.count = -1;
		return;
	}

	struct server_entry *entry = server_entrys[fid];
	pthread_mutex_lock(&entry->lock);
	pthread_rwlock_wrlock(&entry->vnode->rwlock);

	off_t offset = entry->offset;
	ssize_t ret = server_ops.write(entry->vnode->private, offset, count,
				       buf);
	entry->offset += ret;
	if (entry->offset > (off_t)entry->vnode->size)
		entry->vnode->size = entry->offset;

	pthread_rwlock_unlock(&entry->vnode->rwlock);
	pthread_mutex_unlock(&entry->lock);
	pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);

	node->resp.write.count = ret;
}

static void handle_close_direct(struct dq_node *node)
{
	int client_fd = node->req.close.fd;

	int fid = fs_wrapper_get_server_entry(POLLING_CLIENT_BADGE, client_fd);
	if (fid < 0) {
		node->resp.close.ret = -1;
		return;
	}

	struct fs_request fr;
	memset(&fr, 0, sizeof(fr));
	fr.req = FS_REQ_CLOSE;
	fr.close.fd = fid;

	pthread_rwlock_wrlock(&fs_wrapper_meta_rwlock);
	int ret = fs_wrapper_close(NULL, &fr);
	pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);

	node->resp.close.ret = ret;
}

static void handle_request_direct(struct dq_node *node)
{
	switch (node->req.type) {
	case POLLING_FS_REQ_OPEN:
		handle_open_direct(node);
		break;
	case POLLING_FS_REQ_READ:
		handle_read_direct(node);
		break;
	case POLLING_FS_REQ_WRITE:
		handle_write_direct(node);
		break;
	case POLLING_FS_REQ_CLOSE:
		handle_close_direct(node);
		break;
	case POLLING_REQ_EMPTY:
		break;
	case POLLING_KERNEL_REQ_FLUSH_TLB: {
		u64 src_pa = node->req.flush_tlb.memcpy_src_pa;
		u64 dst_pa = node->req.flush_tlb.memcpy_dst_pa;
		u64 len = node->req.flush_tlb.memcpy_len;
		u64 va = node->req.flush_tlb.memcpy_fault_va;
		u64 vmspace = node->req.flush_tlb.memcpy_vmspace;
		int ret = usys_memcpy_and_flush_tlb(src_pa, dst_pa,
						    len, va, vmspace);
		int mid = usys_get_machine_id();
		node->resp.flush_tlb.reply_result = ret;
		node->resp.flush_tlb.reply_from = mid;
		node->resp.flush_tlb.reply_received = 1;
		break;
	}
	default:
		printf("[tmpfs_polling] unsupported request type: %d\n",
		       node->req.type);
		break;
	}
}

/* ================================================================
 * Polling thread main loop
 * ================================================================ */

static void *tmpfs_polling_thread_func(void *arg)
{
	(void)arg;

	int mid = usys_get_machine_id();
	printf("[tmpfs_polling] starting on machine %d\n", mid);
	fflush(stdout);

	void *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
	if (!shm_addr) {
		printf("[tmpfs_polling] failed to allocate vaddr\n");
		return NULL;
	}
	printf("[tmpfs_polling] allocated vaddr at %p\n", shm_addr);
	fflush(stdout);

	int ret = usys_mmap_shm(mid, shm_addr);
	if (ret < 0) {
		printf("[tmpfs_polling] failed to mmap shm id=%d, ret=%d\n",
		       mid, ret);
		return NULL;
	}
	printf("[tmpfs_polling] shm mapped successfully\n");
	fflush(stdout);

	struct polling_shm_region *shm = (struct polling_shm_region *)shm_addr;
	/* SHM is already initialized by the kernel's shm_init(); do not
	 * reinitialize here as it would wipe an already-live queue. */

	printf("[tmpfs_polling] SHM mapped at %p, entering poll loop\n",
	       shm_addr);

	int idle_count = 0;

	while (1) {
		struct dq_node *node = durable_dequeue(shm);

		if (node == NULL) {
			idle_count++;
			if (idle_count > 100) {
				sched_yield();
				idle_count = 0;
			} else {
				__builtin_ia32_pause();
			}
			continue;
		}

		idle_count = 0;
		handle_request_direct(node);

		FLUSH(node);
		atomic_store_explicit(&node->status, DQ_DONE,
				      memory_order_release);
		FLUSH(&node->status);
	}

	return NULL;
}

/* ================================================================
 * Public API
 * ================================================================ */

int tmpfs_start_polling_thread(void)
{
	pthread_t tid;
	int ret = pthread_create(&tid, NULL, tmpfs_polling_thread_func, NULL);
	if (ret != 0) {
		printf("[tmpfs_polling] failed to create thread: %d\n", ret);
		return -1;
	}
	pthread_detach(tid);
	printf("[tmpfs_polling] thread created and detached\n");
	return 0;
}
