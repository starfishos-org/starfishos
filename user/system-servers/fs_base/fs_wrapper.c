#include <errno.h>
#include <pthread.h>
#include <chcore/bug.h>
#include <chcore/type.h>
#include <chcore/memory.h>
#include <chcore-internal/fs_defs.h>
#include <chcore-internal/fs_debug.h>
#include <sys/mman.h>
#include "fs_wrapper_defs.h"
#include "fs_page_cache.h"
#include "fs_vnode.h"
#include "fs_page_fault.h"

#ifdef IPC_PERF_ENABLED
#include <chcore/perf.h>
#endif

/* fs server private data */
struct list_head server_entry_mapping;
pthread_rwlock_t fs_wrapper_meta_rwlock;

/* +++++++++++++++++++++++++++ Initializing +++++++++++++++++++++++++++++++ */

int real_file_reader(char *buffer, pidx_t file_page_idx, void *private)
{
	// page_cache_debug("read page %d block %d into %p\n", b, c, a);
	struct fs_vnode *vnode;
	size_t size;
	off_t offset;

	vnode = (struct fs_vnode *)private;

	size = CACHED_PAGE_SIZE;
	offset = file_page_idx * CACHED_PAGE_SIZE;

	/* buffer size should always be PAGE_SIZE. */
	memset(buffer, 0, size);

	if (offset + size > vnode->size)
		size = vnode->size - offset;
#ifdef TEST_COUNT_PAGE_CACHE
	count.disk_o = count.disk_o + size;
#endif
	// fs_debug("[pc fetch] inode=%ld, offset=%ld, size=%ld, vnode->size=%ld\n",
	// 		vnode->vnode_id, offset, size, vnode->size);
	return server_ops.read(vnode->private, offset, size, buffer);
}

int real_file_writer(char *buffer, pidx_t file_page_idx, int page_block_idx, void *private)
{
	// page_cache_debug("write page %d block %d into %p\n", b, c, a);
	struct fs_vnode *vnode;
	off_t offset;
	size_t size;

	vnode = (struct fs_vnode *)private;
	offset = file_page_idx * CACHED_PAGE_SIZE;
	if (page_block_idx == -1) {
		size = CACHED_PAGE_SIZE;
	} else {
		size = CACHED_BLOCK_SIZE;
		offset += page_block_idx * CACHED_BLOCK_SIZE;
	}

	if (offset + size > vnode->size)
		size = vnode->size - offset;
#ifdef TEST_COUNT_PAGE_CACHE
	count.disk_i = count.disk_i + size;
#endif
	// fs_debug("[pc back] inode=%ld, offset=%ld, size=%ld, vnode->size=%ld\n\n",
	// 		vnode->vnode_id, offset, size, vnode->size);
	return server_ops.write(vnode->private, offset, size, buffer);
}

void init_fs_wrapper(void)
{
	struct user_defined_funcs uf;

	/* fs wrapper */
	init_list_head(&server_entry_mapping);
	fs_vnode_init();
	pthread_rwlock_init(&fs_wrapper_meta_rwlock, NULL);

	uf.file_read = real_file_reader;
	uf.file_write = real_file_writer;
	uf.handler_pce_turns_empty = dec_ref_fs_vnode;
	uf.handler_pce_turns_nonempty = inc_ref_fs_vnode;

	fs_page_cache_init(WRITE_THROUGH, &uf);

#ifdef CHCORE_ENABLE_FMAP
	/* Module: fmap fault */
	fs_page_fault_init();
#endif

#ifdef TEST_COUNT_PAGE_CACHE
	count.hit = 0;
	count.miss = 0;
	count.disk_i = 0;
	count.disk_o = 0;
#endif
}

/* +++++++++++++++++++++++++++ FID Mapping ++++++++++++++++++++++++++++++++ */

/* Get (client_badge, fd) -> fid(server_entry) mapping */
int fs_wrapper_get_server_entry(u64 client_badge, int fd)
{
	struct server_entry_node *n;

	/* Stable fd number, need no translating */
	if (fd == AT_FDROOT)
		return AT_FDROOT;

	/* Validate fd */
	BUG_ON(fd < 0 || fd >= MAX_SERVER_ENTRY_PER_CLIENT);

	for_each_in_list(n, struct server_entry_node, node, &server_entry_mapping)
		if (n->client_badge == client_badge)
			return n->fd_to_fid[fd];

	return -1;
}

/* Set (client_badge, fd) -> fid(server_entry) mapping */
void fs_wrapper_set_server_entry(u64 client_badge, int fd, int fid)
{
	struct server_entry_node *private_iter;

	/* Validate fd */
	BUG_ON(fd < 0 || fd >= MAX_SERVER_ENTRY_PER_CLIENT);

	/* Check if client_badge already involved */
	for_each_in_list(private_iter, struct server_entry_node, node, &server_entry_mapping) {
		if (private_iter->client_badge == client_badge) {
			private_iter->fd_to_fid[fd] = fid;
			return;
		}
	}

	/* New server_entry_node */
	struct server_entry_node *n = (struct server_entry_node *)malloc(sizeof(*n));
	if (n == NULL)
		return;
	n->client_badge = client_badge;
	int i;
	for (i = 0; i < MAX_SERVER_ENTRY_PER_CLIENT; i++)
		n->fd_to_fid[i] = -1;

	n->fd_to_fid[fd] = fid;

	/* Insert node to server_entry_mapping */
	list_append(&n->node, &server_entry_mapping);
}

/* Translate xxxfd field to fid correspondingly */
void translate_fd_to_fid(u64 client_badge, struct fs_request *fr)
{
	/* Except FS_REQ_OPEN, FS_REQ_MOUNT, FS_REQ_BATCH_READ, and FS_REQ_NOOP, fd should be translated */
	if (fr->req == FS_REQ_OPEN || fr->req == FS_REQ_MOUNT
	    || fr->req == FS_REQ_BATCH_READ || fr->req == FS_REQ_NOOP)
		return;

	switch (fr->req) {
	case FS_REQ_FSTATAT:
	case FS_REQ_FSTAT:
	case FS_REQ_FSTATFS:
	case FS_REQ_STATFS:
		fr->stat.dirfd = fs_wrapper_get_server_entry(client_badge, fr->stat.dirfd);
		fr->stat.fd = fs_wrapper_get_server_entry(client_badge, fr->stat.fd);
		break;
	case FS_REQ_READ:
		fr->read.fd = fs_wrapper_get_server_entry(client_badge, fr->read.fd);
		break;
	case FS_REQ_PREAD:
		fr->pread.fd = fs_wrapper_get_server_entry(client_badge, fr->pread.fd);
		break;
	case FS_REQ_PWRITE:
		fr->pwrite.fd = fs_wrapper_get_server_entry(client_badge, fr->pwrite.fd);
		break;
	case FS_REQ_WRITE:
		fr->write.fd = fs_wrapper_get_server_entry(client_badge, fr->write.fd);
		break;
	case FS_REQ_LSEEK:
		fr->lseek.fd = fs_wrapper_get_server_entry(client_badge, fr->lseek.fd);
		break;
	case FS_REQ_CLOSE:
		fr->close.fd = fs_wrapper_get_server_entry(client_badge, fr->close.fd);
		break;
	case FS_REQ_FTRUNCATE:
		fr->ftruncate.fd = fs_wrapper_get_server_entry(client_badge, fr->ftruncate.fd);
		break;
	case FS_REQ_FALLOCATE:
		fr->fallocate.fd = fs_wrapper_get_server_entry(client_badge, fr->fallocate.fd);
		break;
	case FS_REQ_FCNTL:
		fr->fcntl.fd = fs_wrapper_get_server_entry(client_badge, fr->fcntl.fd);
		break;
	case FS_REQ_FSYNC:
		fr->fsync.fd = fs_wrapper_get_server_entry(client_badge, fr->fsync.fd);
		break;
	case FS_REQ_FDATASYNC:
		fr->fdatasync.fd = fs_wrapper_get_server_entry(client_badge, fr->fdatasync.fd);
		break;
#ifdef CHCORE_ENABLE_FMAP
	case FS_REQ_FMAP:
		fr->mmap.fd = fs_wrapper_get_server_entry(client_badge, fr->mmap.fd);
		break;
#endif
	case FS_REQ_GETDENTS64:
		fr->getdents64.fd = fs_wrapper_get_server_entry(client_badge, fr->getdents64.fd);
		break;
	default:
		break;
	}
}

#define IPC_PERF_TIME_SIZE 10240
u64 fs_server_dispatch_begin_time[IPC_PERF_TIME_SIZE];
u64 fs_server_dispatch_end_time[IPC_PERF_TIME_SIZE];
u64 fs_server_dispatch_begin_count = 0;
u64 fs_server_dispatch_end_count = 0;

void fs_server_dispatch(ipc_msg_t *ipc_msg, u64 client_badge)
{
	#ifdef IPC_PERF_ENABLED
	fs_server_dispatch_begin_time[fs_server_dispatch_begin_count++] = rdtsc();
	#endif

	struct fs_request *fr;
	long ret;
	bool ret_with_cap = false;

	fr = (struct fs_request *)ipc_get_msg_data(ipc_msg);

	/* We only support concurrent READ, WRITE, BATCH_READ, and NOOP */
	if (fr->req != FS_REQ_READ && fr->req != FS_REQ_WRITE
	    && fr->req != FS_REQ_BATCH_READ && fr->req != FS_REQ_NOOP) {
		pthread_rwlock_wrlock(&fs_wrapper_meta_rwlock);
	} else {
		pthread_rwlock_rdlock(&fs_wrapper_meta_rwlock);
	}

	/*
	 * Some FS Servers need to complete the initialization process when mounting
	 * eg. Connect with corresponding block device, Save partition offset, etc
	 * So, when the mounted flag is off, requests will be rejected except FS_REQ_MOUNT
	 */
	if (!mounted && (fr->req != FS_REQ_MOUNT)) {
		printf("[fs server] Not fully initialized, send FS_REQ_MOUNT first\n");
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Now fr->fd stores the `Client Side FD Index',
	 * We need to translate fr->fd to fid here, except FS_REQ_OPEN
	 * FS_REQ_OPEN's fr->fd stores the newly generated `Client Side FD Index'
	 * and we should build mapping between fr->fd to fid when handle open request
	 */
	translate_fd_to_fid(client_badge, fr);

	/*
	 * FS Server Requests Handlers
	 */
	switch(fr->req) {
	case FS_REQ_MOUNT:
		ret = fs_wrapper_mount(ipc_msg, fr);
		break;
	case FS_REQ_UMOUNT:
		ret = fs_wrapper_umount(ipc_msg, fr);
		break;
	case FS_REQ_OPEN:
		ret = fs_wrapper_open(client_badge, ipc_msg, fr);
		break;
	case FS_REQ_READ:
		ret = fs_wrapper_read(ipc_msg, fr);
		break;
	case FS_REQ_PREAD:
		ret = fs_wrapper_pread(ipc_msg, fr);
		break;
	case FS_REQ_PWRITE:
		ret = fs_wrapper_pwrite(ipc_msg, fr);
		break;
	case FS_REQ_WRITE:
		ret = fs_wrapper_write(ipc_msg, fr);
		break;
	case FS_REQ_LSEEK:
		ret = fs_wrapper_lseek(ipc_msg, fr);
		break;
	case FS_REQ_CLOSE:
		ret = fs_wrapper_close(ipc_msg, fr);
		break;
	case FS_REQ_CREAT:
		ret = fs_wrapper_creat(ipc_msg, fr);
		break;
	case FS_REQ_UNLINK:
		ret = fs_wrapper_unlink(ipc_msg, fr);
		break;
	case FS_REQ_RMDIR:
		ret = fs_wrapper_rmdir(ipc_msg, fr);
		break;
	case FS_REQ_MKDIR:
		ret = fs_wrapper_mkdir(ipc_msg, fr);
		break;
	case FS_REQ_RENAME:
		ret = fs_wrapper_rename(ipc_msg, fr);
		break;
	case FS_REQ_GETDENTS64:
		ret = fs_wrapper_getdents64(ipc_msg, fr);
		break;
	case FS_REQ_FTRUNCATE:
		ret = fs_wrapper_ftruncate(ipc_msg, fr);
		break;
	case FS_REQ_FSTATAT:
		ret = fs_wrapper_fstatat(ipc_msg, fr);
		break;
	case FS_REQ_FSTAT:
		ret = fs_wrapper_fstat(ipc_msg, fr);
		break;
	case FS_REQ_STATFS:
		ret = fs_wrapper_statfs(ipc_msg, fr);
		break;
	case FS_REQ_FSTATFS:
		ret = fs_wrapper_fstatfs(ipc_msg, fr);
		break;
	case FS_REQ_FACCESSAT:
		ret = fs_wrapper_faccessat(ipc_msg, fr);
		break;
	case FS_REQ_SYMLINKAT:
		ret = fs_wrapper_symlinkat(ipc_msg, fr);
		break;
	case FS_REQ_READLINKAT:
		ret = fs_wrapper_readlinkat(ipc_msg, fr);
		break;
	case FS_REQ_FALLOCATE:
		ret = fs_wrapper_fallocate(ipc_msg, fr);
		break;
	case FS_REQ_FCNTL:
		ret = fs_wrapper_fcntl(client_badge, ipc_msg, fr);
		break;
#ifdef CHCORE_ENABLE_FMAP
	case FS_REQ_FMAP:
		ret = fs_wrapper_fmap(client_badge, ipc_msg, fr, &ret_with_cap);
		break;
#endif
	case FS_REQ_SYNC:
		ret = fs_wrapper_sync();
		break;
	case FS_REQ_FSYNC:
	case FS_REQ_FDATASYNC:
		/* TODO: Currently, fdatasync() behaves the same as fsync(). */
		ret = fs_wrapper_fsync(ipc_msg, fr);
		break;
	case FS_REQ_TEST_PERF:
		ret = fs_wrapper_count(ipc_msg, fr);
		break;
	case FS_REQ_BATCH_READ:
		ret = fs_wrapper_batch_read(client_badge, ipc_msg);
		break;
	case FS_CHILD_FINISH_FORK:
		ret = fs_finish_fork(ipc_msg, fr->fork.childBadge, fr->fork.parentBagde);
		break;
	case FS_REQ_NOOP:
		ret = 0;
		break;
#ifdef IPC_PERF_ENABLED
	case FS_REQ_IPC_PERF:
		ret = fs_wrapper_ipc_perf(ipc_msg, fr);
		break;
#endif
	default:
		printf("[Error] Strange FS Server request number %d\n", fr->req);
		ret = -EINVAL;
		break;
	}

out:
	pthread_rwlock_unlock(&fs_wrapper_meta_rwlock);
	#ifdef IPC_PERF_ENABLED
	fs_server_dispatch_end_time[fs_server_dispatch_end_count++] = rdtsc();
	#endif
	if (ret_with_cap)
		ipc_return_with_cap(ipc_msg, ret);
	else
		ipc_return(ipc_msg, ret);
}
