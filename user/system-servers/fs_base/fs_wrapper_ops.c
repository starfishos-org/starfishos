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

/* Return true if fd is NOT valid */
static inline bool fd_type_invalid(int fd, bool isfile)
{
	if (fd < 0 || fd >= MAX_SERVER_ENTRY_NUM)
		return true;
	if (server_entrys[fd] == NULL)
		return true;
	if (isfile && (server_entrys[fd]->vnode->type != FS_NODE_REG))
		return true;
	if (!isfile && (server_entrys[fd]->vnode->type != FS_NODE_DIR))
		return true;
	return false;
}

static int get_path_leaf(const char *path, char *path_leaf)
{
	int i;
	int ret;

	ret = -1; /* return -1 means find no '/' */

	for (i = strlen(path) - 2; i >= 0; i--) {
		if (path[i] == '/') {
			strcpy(path_leaf, path + i + 1);
			ret = 0;
			break;
		}
	}

	if (ret == -1)
		return ret;

	if (path_leaf[strlen(path_leaf) - 1] == '/') {
		path_leaf[strlen(path_leaf) - 1] = '\0';
	}

	return ret;
}

/* TODO: path should be a normalized path */
static int get_path_prefix(const char *path, char *path_prefix)
{
	int i;
	int ret;

	ret = -1; /* return -1 means find no '/' */

	BUG_ON(strlen(path) > FS_REQ_PATH_BUF_LEN);

	strcpy(path_prefix, path);
	for (i = strlen(path_prefix) -  2; i >= 0; i--) {
		if (path_prefix[i] == '/') {
			path_prefix[i] = '\0';
			ret = 0;
			break;
		}
	}

	return ret;
}

static int check_path_leaf_is_not_dot(const char *path)
{
	char leaf[FS_REQ_PATH_BUF_LEN];

	if (get_path_leaf(path, leaf) == -1)
		return -EINVAL;
	if (strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0)
		return -EINVAL;

	return 0;
}

/* Default server operation: do nothing, just print error info and return -1 */
int default_server_operation(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	printf("[fs server] operation %d is not defined\n", fr->req);
	return -1;
}

ssize_t default_ssize_t_server_operation(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	printf("[fs server] operation %d is not defined\n", fr->req);
	return -1;
}

int fs_wrapper_open(u64 client_badge, ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int new_fd;
	char *path;
	int flags;
	int mode;
	int entry_id;
	int ret;

	ino_t vnode_id;
	int vnode_type;
	size_t vnode_size;
	void *vnode_private;

	struct fs_vnode *vnode;

	off_t entry_off;

	/* Prase arguments from fr */
	new_fd = fr->open.new_fd; /* Store fr->fd (newly generated client fd) to new_fd temporarly */
	path = fr->open.pathname;
	flags = fr->open.flags;
	mode = fr->open.mode;

	fs_debug_trace_fswrapper("new_fd=%d, flags=0%o, path=%s\n", new_fd, flags, path);

	fr->open.new_fd = AT_FDROOT;
	ret = server_ops.open(path, flags, mode, &vnode_id, &vnode_size, &vnode_type, &vnode_private);
	if (ret != 0) {
		fs_debug_error("ret = %d\n", ret);
		return ret;
	}

	fs_debug_trace_fswrapper("vnode_id=%ld, vnode_size=0x%lx, vnode_type=%d\n",
							  vnode_id,
							  vnode_size,
							  vnode_type);

	if (vnode_private && (flags & O_CREAT) && (flags & O_EXCL)) {
		server_ops.close(vnode_private, (vnode_type == FS_NODE_DIR));
		return -EEXIST;
	}

	if ((flags & O_DIRECTORY) && vnode_type != FS_NODE_DIR) {
		server_ops.close(vnode_private, (vnode_type == FS_NODE_DIR));
		return -ENOTDIR;
	}

	if ((flags & (O_RDWR | O_WRONLY)) && vnode_type == FS_NODE_DIR) {
		server_ops.close(vnode_private, (vnode_type == FS_NODE_DIR));
		return -ENOTDIR;
	}

	if (flags & O_NOCTTY) {
		// TODO: O_NOCTTY not supported
		BUG_ON(1);
	}

	if (!(flags & (O_RDWR | O_WRONLY)) && (flags & (O_TRUNC | O_APPEND))) {
		server_ops.close(vnode_private, (vnode_type == FS_NODE_DIR));
		return -EACCES;
	}

	// TODO(TCZ): check file permission with flag, return EACCES on mismatch
	if (flags & O_TRUNC && vnode_type == FS_NODE_REG) {
		server_ops.ftruncate(vnode_private, 0);
	}

	entry_id = alloc_entry();
	if (entry_id < 0) {
		server_ops.close(vnode_private, (vnode_type == FS_NODE_DIR));
		return -EMFILE;
	}

	if (flags & O_APPEND && vnode_type == FS_NODE_REG) {
		entry_off = vnode_size;
	} else {
		entry_off = 0;
	}

	vnode = get_fs_vnode_by_id(vnode_id);
	if (NULL != vnode) {
		/* Assign new entry to existing vnode, close newly opened struct */
		inc_ref_fs_vnode(vnode);
		assign_entry(server_entrys[entry_id], flags, entry_off, 1, (void *)strdup(path), vnode);
		server_ops.close(vnode_private, (vnode_type == FS_NODE_DIR));
	} else {
		vnode = alloc_fs_vnode(vnode_id, vnode_type, vnode_size, vnode_private);
		push_fs_vnode(vnode);
		assign_entry(server_entrys[entry_id], flags, entry_off, 1, (void *)strdup(path), vnode);
	}

	/* After server handling the open request, mapping new_fd to fid */
	fs_wrapper_set_server_entry(client_badge, new_fd, entry_id);

	fs_debug_trace_fswrapper("entry_id=%d\n", entry_id);

	return new_fd;
}

int fs_wrapper_close(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int fd;
	struct fs_vnode *vnode;
	fs_debug_trace_fswrapper("fd=%d\n", fr->close.fd);

	/* Parsing and check arguments */
	fd = fr->close.fd;
	if (fd_type_invalid(fd, true) && fd_type_invalid(fd, false)) {
		fs_debug_error("fd_type_invalid\n");
		return -ENOENT;
	}

	vnode = server_entrys[fd]->vnode;
	server_entrys[fd]->refcnt--;
	if (server_entrys[fd]->refcnt == 0) {
		free_entry(fd);
		dec_ref_fs_vnode(vnode);
	}

	/*
	 * To preserve page cache even after we close the file,
	 * we don't revoke vnode when user call close().
	 */

	/*
	 * TODO(HYB): Note that now the vnode is only revoked when unlink() is called.
	 * So what is the best time to free the vnode ?
	 */

	/* Revoke vnode, if refcnt == 0 */

	return 0;
}

int fs_wrapper_read(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int fd;
	char *buf;
	off_t offset;
	size_t size;
	void *operator;
	int ret;
	struct fs_vnode *vnode;
	char *page_buf;
	int fptr, page_idx, page_off, copy_size;

	ret = 0;
	fd = fr->read.fd;
	buf = (void *)fr;
	fs_debug_trace_fswrapper("entry_id=%d\n", fd);

	pthread_mutex_lock(&server_entrys[fd]->lock);
	pthread_rwlock_rdlock(&server_entrys[fd]->vnode->rwlock);

	size = (size_t)fr->read.count;
	offset = (off_t)server_entrys[fd]->offset;
	vnode = server_entrys[fd]->vnode;
	operator = server_entrys[fd]->vnode->private;

	/*
	* If offset is already outside the file,
	*      do nothing and return 0
	*/
	if (offset >= server_entrys[fd]->vnode->size) {
		goto out;
	}

	/*
	* If offset + size > file_size,
	* 	change size to (file_size - offset).
	*/
	if (offset + size > server_entrys[fd]->vnode->size) {
		size = server_entrys[fd]->vnode->size - offset;
	}

	/*
	* Server-side read operation should implement like:
	* - Base: read file from `offset` for `size` length,
	*      if it touch a file ending, return content from offset to end
	*      and return bytes read.
	*/
	if (!using_page_cache) {
		ret = server_ops.read(operator, offset, size, buf);
	} else {
		for (fptr = offset; fptr < offset + size; fptr = ROUND_DOWN(fptr, PAGE_SIZE) + PAGE_SIZE) {
			page_idx = fptr / PAGE_SIZE;
			page_off = fptr - ROUND_DOWN(fptr, PAGE_SIZE);
			copy_size = MIN(PAGE_SIZE - page_off, offset + size - fptr);

			/* get-read-put */
			page_buf = page_cache_get_block_or_page(vnode->page_cache, page_idx, -1, READ);
			memcpy(buf + fptr - offset, page_buf + page_off, copy_size);
			page_cache_put_block_or_page(vnode->page_cache, page_idx, -1, READ);

			ret += copy_size;
		}
	}

	/* Update server_entry and vnode metadata */
	server_entrys[fd]->offset += ret;

out:
	pthread_rwlock_unlock(&server_entrys[fd]->vnode->rwlock);
	pthread_mutex_unlock(&server_entrys[fd]->lock);
	return ret;
}

int fs_wrapper_pread(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int fd;
	char *buf;
	off_t offset;
	size_t size;
	void *operator;
	int ret;
	struct fs_vnode *vnode;
	char *page_buf;
	int fptr, page_idx, page_off, copy_size;

	ret = 0;
	fd = fr->pread.fd;
	buf = (void *)fr;
	fs_debug_trace_fswrapper("entry_id=%d\n", fd);

	pthread_mutex_lock(&server_entrys[fd]->lock);
	pthread_rwlock_rdlock(&server_entrys[fd]->vnode->rwlock);

	size = (size_t)fr->pread.count;
	offset = (off_t)fr->pread.offset;
	vnode = server_entrys[fd]->vnode;
	operator = server_entrys[fd]->vnode->private;

	/*
	* If offset is already outside the file,
	*      do nothing and return 0
	*/
	if (offset >= server_entrys[fd]->vnode->size) {
		goto out;
	}

	/*
	* If offset + size > file_size,
	* 	change size to (file_size - offset).
	*/
	if (offset + size > server_entrys[fd]->vnode->size) {
		size = server_entrys[fd]->vnode->size - offset;
	}

	/*
	* Server-side read operation should implement like:
	* - Base: read file from `offset` for `size` length,
	*      if it touch a file ending, return content from offset to end
	*      and return bytes read.
	*/
	if (!using_page_cache) {
		ret = server_ops.read(operator, offset, size, buf);
	} else {
		for (fptr = offset; fptr < offset + size; fptr = ROUND_DOWN(fptr, PAGE_SIZE) + PAGE_SIZE) {
			page_idx = fptr / PAGE_SIZE;
			page_off = fptr - ROUND_DOWN(fptr, PAGE_SIZE);
			copy_size = MIN(PAGE_SIZE - page_off, offset + size - fptr);

			/* get-read-put */
			page_buf = page_cache_get_block_or_page(vnode->page_cache, page_idx, -1, READ);
			memcpy(buf + fptr - offset, page_buf + page_off, copy_size);
			page_cache_put_block_or_page(vnode->page_cache, page_idx, -1, READ);

			ret += copy_size;
		}
	}

	/* Update server_entry and vnode metadata */
	server_entrys[fd]->offset = offset + ret;

out:
	pthread_rwlock_unlock(&server_entrys[fd]->vnode->rwlock);
	pthread_mutex_unlock(&server_entrys[fd]->lock);
	return ret;
}

int fs_wrapper_write(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int fd;
	char *buf;
	size_t size;
	off_t offset;
	void *operator;
	int ret;
	struct fs_vnode *vnode;
	char *block_buf;
	int fptr, page_idx, block_idx, block_off, copy_size;

	ret = 0;
	fd = fr->write.fd;
	buf = (void *)fr + sizeof(struct fs_request);
	fs_debug_trace_fswrapper("entry_id=%d\n", fd);

	pthread_mutex_lock(&server_entrys[fd]->lock);
	pthread_rwlock_wrlock(&server_entrys[fd]->vnode->rwlock);

	size = (size_t)fr->write.count;
	offset = (off_t)server_entrys[fd]->offset;
	vnode = server_entrys[fd]->vnode;
	operator = server_entrys[fd]->vnode->private;

	/*
	* If size == 0, do nothing and return 0
	* Even the offset is outside of the file, inode size is not changed!
	*/
	if (size == 0) {
		goto out;
	}

	/*
	* Server-side write operation should implement like:
	* - Base: write file and return bytes written
	* - If offset is outside the file (notice size=0 is handled)
	*      Filling '\0' until offset pos, then append file
	*/

	if (!using_page_cache)
		ret = server_ops.write(operator, offset, size, buf);
	else {
		if (offset + size > vnode->size) {
			vnode->size = offset + size;
			server_ops.ftruncate(operator, offset + size);
		}
		for (fptr = offset; fptr < offset + size; fptr = ROUND_DOWN(fptr, CACHED_BLOCK_SIZE) + CACHED_BLOCK_SIZE) {
			page_idx = fptr / CACHED_PAGE_SIZE;
			block_idx = (fptr - ROUND_DOWN(fptr, PAGE_SIZE)) / CACHED_BLOCK_SIZE;
			block_off = fptr - ROUND_DOWN(fptr, CACHED_BLOCK_SIZE);
			copy_size = MIN(CACHED_BLOCK_SIZE - block_off, offset + size - fptr);

			/* get-write-put */
			block_buf = page_cache_get_block_or_page(vnode->page_cache, page_idx, block_idx, WRITE);
			memcpy(block_buf + block_off, buf + fptr - offset, copy_size);
			page_cache_put_block_or_page(vnode->page_cache, page_idx, block_idx, WRITE);

			ret += copy_size;
		}
	}

	/* Update server_entry and vnode metadata */
	server_entrys[fd]->offset += ret;
	if (server_entrys[fd]->offset > server_entrys[fd]->vnode->size) {
		server_entrys[fd]->vnode->size = server_entrys[fd]->offset;
	}

out:
	pthread_rwlock_unlock(&server_entrys[fd]->vnode->rwlock);
	pthread_mutex_unlock(&server_entrys[fd]->lock);
	return ret;
}

int fs_wrapper_lseek(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int fd;
	off_t offset;
	int whence;
	off_t target_off;

	fd = fr->lseek.fd;
	offset = fr->lseek.offset;
	whence = fr->lseek.whence;

	switch (whence) {
	case SEEK_SET: {
		target_off = offset;
		break;
	}
	case SEEK_CUR: {
		target_off = server_entrys[fd]->offset + offset;
		break;
	}
	case SEEK_END:
		target_off = server_entrys[fd]->vnode->size + offset;
		break;
	default: {
		printf("%s: %d Not impelemented yet\n", __func__, whence);
		target_off = -1;
		break;
	}
	}
	if (target_off < 0)
		return -EINVAL;

	server_entrys[fd]->offset = target_off;

	return target_off;
}

int fs_wrapper_ftruncate(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int ret;
	int fd;
	off_t len;
	void *operator;

	fd = fr->ftruncate.fd;
	if (fd_type_invalid(fd, true)) {
		return -EBADF;
	}

	/* TODO: Check fd flags PRECISELY */
	if (!(server_entrys[fd]->flags & O_WRONLY) && !(server_entrys[fd]->flags & O_RDWR)) {
		return -EINVAL;
	}

	len = fr->ftruncate.length;

	/* The argument len is negative or larger than the maximum file size */
	if (len < 0)
		return -EINVAL;

	operator = server_entrys[fd]->vnode->private;

	ret = server_ops.ftruncate(operator, len);
	if (!ret)
		server_entrys[fd]->vnode->size = len;
	return ret;
}

int fs_wrapper_fstatat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	BUG_ON(fr->stat.dirfd != AT_FDROOT);
	char *path = fr->stat.pathname;
	int flags = fr->stat.flags;
	struct stat *st = (struct stat *)ipc_get_msg_data(ipc_msg);
	int err;
	fs_debug_trace_fswrapper("path=%s, flags=%d\n", path, flags);

	err = server_ops.fstatat(path, st, flags);
	if (err)
		return err;

	struct fs_vnode *vnode;
	vnode = get_fs_vnode_by_id(st->st_ino);
	if (vnode && (st->st_mode & S_IFREG)) {
		/* vnode is cached in memory, update size in stat */
		st->st_size = vnode->size;
	}

	return 0;
}

int fs_wrapper_unlink(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	char *path = fr->unlink.pathname;
	int flags = fr->unlink.flags;
	int ret;
	struct stat st;
	struct fs_vnode *vnode = NULL;
	fs_debug_trace_fswrapper("path=%s, flags=0%o\n", path, flags);

	if (using_page_cache) {
		/* clear page cache */
		ret = server_ops.fstatat(path, &st, AT_SYMLINK_NOFOLLOW);
		if (ret)
			return ret;
		vnode = get_fs_vnode_by_id(st.st_ino);
		if (vnode)
			page_cache_delete_pages_of_inode(vnode->page_cache);
	}

	vnode = get_fs_vnode_by_id(st.st_ino);
	if (vnode && vnode->refcnt != 0) {
		// printf("Warning: unlink while vnode refcnt != 0\n");
	}

	ret = server_ops.unlink(path, flags);

	return ret;
}

int fs_wrapper_rename(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int ret;
	char *oldpath = fr->rename.oldpath;
	char *newpath = fr->rename.newpath;
	char new_path_prefix[FS_REQ_PATH_BUF_LEN];
	struct stat st;
	struct fs_vnode *vnode;
	bool old_is_dir, new_is_dir;
	ino_t old_ino;
	fs_debug_trace_fswrapper("old=%s, new=%s\n", oldpath, newpath);

	/* Check . and .. in the final component */
	if ((ret = check_path_leaf_is_not_dot(oldpath)) != 0)
		return ret;
	if ((ret = check_path_leaf_is_not_dot(newpath)) != 0)
		return ret;

	/* Check if oldpath exists */
	ret = server_ops.fstatat(oldpath, &st, AT_SYMLINK_NOFOLLOW);
	if (ret == -ENOENT)
		return ret;
	else if (ret != 0) {
		printf("ret=%d\n", ret);
		BUG_ON(1);
	}

	old_is_dir = (st.st_mode & S_IFDIR) ? true : false;
	old_ino = st.st_ino;

	/* Check old is not a ancestor of new */
	if (strncmp(oldpath, newpath, strlen(oldpath)) == 0) {
		if (newpath[strlen(oldpath)] == '/')
			return -EINVAL;
	}

	/* Check if new_path_prefix valid*/
	if (get_path_prefix(newpath, new_path_prefix) == -1) {
		return -EINVAL;
	}
	if (new_path_prefix[0]) {
		ret = server_ops.fstatat(new_path_prefix, &st, AT_SYMLINK_NOFOLLOW);
		if (ret == -ENOENT) {
			return ret;
		} else if (ret != 0) {
			BUG_ON(ret);
		}

		if (!(st.st_mode & S_IFDIR))
			return -ENOTDIR;
	}

	/* If oldpath and newpath both exists */
	ret = server_ops.fstatat(newpath, &st, AT_SYMLINK_NOFOLLOW);
	if (ret != -ENOENT) {
		new_is_dir = (st.st_mode & S_IFDIR) ? true : false;
		if (old_is_dir && !new_is_dir)
			return -ENOTDIR;
		if (!old_is_dir && new_is_dir)
			return -EISDIR;
		if (old_is_dir) {
			/* both old and new are dirs */
			ret = server_ops.rmdir(newpath, AT_SYMLINK_NOFOLLOW);
			if (ret == -ENOTEMPTY)
				return ret;
			BUG_ON(ret);
		} else {
			/* both regular */
			ret = server_ops.unlink(newpath, AT_SYMLINK_NOFOLLOW);
			if (ret)
				return ret;
			BUG_ON(ret);
		}
	}

	/* Flush page cache of oldpath */
	if (using_page_cache && !old_is_dir) {
		/* clear page cache */
		vnode = get_fs_vnode_by_id(old_ino);
		if (vnode)
			page_cache_evict_pages_of_inode(vnode->page_cache);
	}

	ret = server_ops.rename(oldpath, newpath);

	return ret;
}

int fs_wrapper_count(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	printf("hit: %d miss: %d disk_writer: %d disk_read: %d\n",
	       count.hit,
	       count.miss,
	       count.disk_i,
	       count.disk_o);
	return 0;
}

int fs_wrapper_rmdir(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	char *path = fr->rmdir.pathname;
	int flags = fr->rmdir.flags;
	fs_debug_trace_fswrapper("path=%s, flags=0%o\n", path, flags);

	return server_ops.rmdir(path, flags);
}

int fs_wrapper_mkdir(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	int ret;

	const char *path = fr->mkdir.pathname;
	mode_t mode = fr->mkdir.mode;
	fs_debug_trace_fswrapper("path=%s, mode=%d\n", path, mode);

	ret = server_ops.mkdir(path, mode);
	return ret;
}

int fs_wrapper_sync(void)
{
	int ret = 0;

	if (using_page_cache)
		ret = page_cache_flush_all_pages();

	return ret;
}

int fs_wrapper_fsync(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	struct fs_vnode *vnode;
	int ret = 0;

	int fd = fr->fsync.fd;

	BUG_ON(fd == AT_FDROOT);

	if (using_page_cache) {
		vnode = server_entrys[fd]->vnode;
		ret = page_cache_flush_pages_of_inode(vnode->page_cache);
	}

	return ret;
}

#ifdef CHCORE_ENABLE_FMAP
int fs_wrapper_fmap(u64 client_badge, ipc_msg_t *ipc_msg, struct fs_request *fr, bool *ret_with_cap)
{
	void *addr;
	size_t length;
	int prot;
	int flags;
	int fd;
	off_t offset;
	struct fs_vnode *vnode;
	int pmo_cap;
	int ret;

	/* If there is no valid fmap implementation, return -EINVAL */
	if (!using_page_cache && server_ops.fmap_get_page_addr == default_fmap_get_page_addr) {
		fs_debug_error("fmap is not impl.\n");
		return -EINVAL;
	}

	/* Step: Parsing arguments in fr */
	addr = (void *)fr->mmap.addr;
	length = (size_t)fr->mmap.length;
	prot = fr->mmap.prot;
	flags = fr->mmap.flags;
	fd = fr->mmap.fd;
	offset = (off_t)fr->mmap.offset;

	vnode = server_entrys[fd]->vnode;

	fs_debug_trace_fswrapper(
		"addr=0x%lx, length=0x%lx, prot=%d, flags=%d, fd=%d, offset=0x%lx\n",
		(u64)addr, length, prot, flags, fd, offset);

	/* Sanity Check for arguments */
	if (prot & (~(PROT_NONE | PROT_READ | PROT_WRITE | PROT_EXEC))) {
		return -EINVAL;
	}

	if (flags & MAP_ANONYMOUS) {
		return -EINVAL;
	}

	if (flags != MAP_SHARED && flags != MAP_PRIVATE) {
		/**
		 * TODO: We don't support MAP_FIXED here
		 * TODO: Some comment dropped in old sys_handle_fmap.
		 * kwarn("%s: fmap only supports MAP_SHARED, 0x%x.\n"
		      "But we allow other values for fast dev."
		      " Shall we make this CoW?\n",
		      __func__, flags);
		 */
		fs_debug_error("unsupported flags=%d\n", flags);
		return -EINVAL;
	}

	if (length % PAGE_SIZE) {
		return -EINVAL;
	}
	UNUSED(addr); UNUSED(fd); UNUSED(offset);

	/* Step: Create a PMO_FILE for file, if not created */
	if (vnode->pmo_cap == -1) {
		/* FIXME: what about file size changed? */
		pmo_cap = usys_create_pmo(vnode->size, PMO_FILE, MALLOC_TYPE_DEFAULT);
		if (pmo_cap < 0) {
			ret = pmo_cap;
			goto out_fail;
		}
		vnode->pmo_cap = pmo_cap;
	}

	/* Step: Record (client_badge, mmaped_va, length) -> (vnode, offset, flags) */
	ret = fmap_area_insert(client_badge, (vaddr_t)addr, length,
		vnode, (size_t)offset, flags);
	if (ret < 0) {
		goto out_fail;
	}

	/* Step: Send PMO_FILE back to client side */
	ipc_msg->cap_slot_number = 1;
	ipc_set_msg_cap(ipc_msg, 0, vnode->pmo_cap);
	*ret_with_cap = true;

	return 0;
out_fail:
	*ret_with_cap = false;
	return ret;
}
#endif

int fs_wrapper_creat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.creat(ipc_msg, fr);
}

int fs_wrapper_getdents64(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.getdents64(ipc_msg, fr);
}

int fs_wrapper_fstat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.fstat(ipc_msg, fr);
}

int fs_wrapper_statfs(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.statfs(ipc_msg, fr);
}

int fs_wrapper_fstatfs(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.fstatfs(ipc_msg, fr);
}

int fs_wrapper_faccessat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	fs_debug_trace_fswrapper("path=%s\n", fr->faccessat.pathname);
	return server_ops.faccessat(ipc_msg, fr);
}

int fs_wrapper_symlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.symlinkat(ipc_msg, fr);
}

int fs_wrapper_readlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.readlinkat(ipc_msg, fr);
}

int fs_wrapper_fallocate(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.fallocate(ipc_msg, fr);
}

int fs_wrapper_fcntl(u64 client_badge, ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	struct server_entry *entry;
	void* operator;
	int ret = 0;

	if ((entry = server_entrys[fr->fcntl.fd]) == NULL)
		return -EBADF;

	switch (fr->fcntl.fcntl_cmd) {
	case F_GETFL:
		ret = entry->flags;
		break;
	case F_SETFL: {
		// file access mode and the file creation flags
		// should be ingored, per POSIX
		int effective_bits = (~O_ACCMODE & ~O_CLOEXEC & ~O_CREAT &
				      ~O_DIRECTORY & ~O_EXCL & ~O_NOCTTY &
				      ~O_NOFOLLOW & ~O_TRUNC & ~O_TTY_INIT);

		entry->flags = (fr->fcntl.fcntl_arg & effective_bits) |
			     (entry->flags & ~effective_bits);
		break;
	}
	case F_DUPFD:{
		fs_wrapper_set_server_entry(client_badge, fr->fcntl.fcntl_arg, fr->fcntl.fd);
		server_entrys[fr->fcntl.fd]->refcnt++;
		operator = entry->vnode->private;
		ret = server_ops.fcntl(operator, fr->fcntl.fd, fr->fcntl.fcntl_cmd, fr->fcntl.fcntl_arg);
		break;
	}

	case F_GETOWN:
	case F_SETOWN:
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
	default:
		printf("unsopported fcntl cmd %d\n", fr->fcntl.fcntl_cmd);
		ret = -1;
		break;
	}

	return ret;
}

int fs_wrapper_mount(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	/*
	 * Mount req should be called only if mounted flag is off,
	 * Normally, only called once after booted during FSM's mount procedure
	 */
	int ret;
	if (mounted) {
		printf("[Error] fs: server has been mounted!\n");
		ret = -EINVAL;
		goto out;
	}
	ret = server_ops.mount(ipc_msg, fr);
	if (!ret)
		mounted = true;
out:
	return ret;
}

int fs_wrapper_umount(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return server_ops.umount(ipc_msg, fr);
}

int fs_finish_fork(ipc_msg_t *ipc_msg, u64 child_badge, u64 parent_badge) 
{
	struct fmap_area_mapping *area_iter;
	struct server_entry_node *private_iter;
	int ret;

	/* Check if client_badge already involved */
	for_each_in_list(private_iter, struct server_entry_node, node, &server_entry_mapping) {
		if (private_iter->client_badge == parent_badge) {
			/* New server_entry_node */
			struct server_entry_node *n = (struct server_entry_node *)malloc(sizeof(*n));
			n->client_badge = child_badge;
			int i;
			/* Insert node to server_entry_mapping */
			for (i = 0; i < MAX_SERVER_ENTRY_PER_CLIENT; i++) {
				int fid;
				if((fid = private_iter->fd_to_fid[i]) != -1) {
					pthread_mutex_lock(&server_entrys[fid]->lock);
					server_entrys[fid]->refcnt++;
					n->fd_to_fid[i] = fid;
					pthread_mutex_unlock(&server_entrys[fid]->lock);
				}
			}
			list_append(&n->node, &server_entry_mapping);
			break;
		}
	}

	for_each_in_list (area_iter,
			  struct fmap_area_mapping,
			  node,
			  &fmap_area_mappings) {
		if (area_iter->client_badge == parent_badge) {
			pthread_rwlock_rdlock(&area_iter->vnode->rwlock);
			ret = fmap_area_insert(child_badge,area_iter->client_va_start,
				area_iter->length,area_iter->vnode,area_iter->file_offset,area_iter->flags);
			if (ret < 0) {
				pthread_rwlock_unlock(&area_iter->vnode->rwlock);
				goto out_fail;
			}
			// area_iter->vnode->refcnt += 1;
			pthread_rwlock_unlock(&area_iter->vnode->rwlock);
		}
	}
	return 0;
out_fail:
	return ret;
}
