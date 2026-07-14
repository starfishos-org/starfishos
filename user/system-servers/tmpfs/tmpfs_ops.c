#include <chcore/cpio.h>
#include <chcore/defs.h>
#include <chcore/falloc.h>
#include <chcore-internal/fs_defs.h>
#include <chcore/ipc.h>
#include <chcore/launch_kern.h>
#include <chcore/syscall.h>
#include <chcore/memory.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/mman.h>
#include <time.h>
#include <limits.h>

#include <chcore-internal/fs_defs.h>
#include <chcore-internal/fs_debug.h>
#include <fs_wrapper_defs.h>
#include <fs_vnode.h>
#include "tmpfs.h"
#include "plog.h"
#include "cxlfs.h"

#define DIRENT_NAME_MAX 256


int __fs_openat_callback(int retcode, struct tmpfs_walk_path *walk, void *data)
{
	int flags = ((struct openat_callback_args *)data)->flags;
	mode_t mode = ((struct openat_callback_args *)data)->mode;
	if (flags & O_CREAT) {
		const char *path = ((struct openat_callback_args *)data)->path;
		if (!plog_is_replaying() && plog_append_creat(path, mode) < 0)
			return -ENOSPC;
		return tfs_creat(path_last_inode(&walk->path_record),
				 walk->leaf, strlen(walk->leaf),
				 mode, &(walk->target_dent));
	}
	return retcode;
}

int tmpfs_open(char *path, int flags, int mode, ino_t *vnode_id, size_t *vnode_size, int *vnode_type, void **vnode_private)
{
	struct tmpfs_walk_path walk;
	struct openat_callback_args callback_data;
	int err;

	fs_debug("path=%s, flags=%d, mode=%d\n", path, flags, mode);

	walk.path = path;
	walk.path_len = strlen(path);

	callback_data.flags = flags;
	callback_data.mode = mode;
	callback_data.path = path;
	walk.not_found_callback = __fs_openat_callback;
	walk.callback_data = &callback_data;
	walk.follow_symlink = flags & O_NOFOLLOW ? 0 : 1;
	err = handle_xxxat(AT_FDROOT, &walk);
	if (err)
		goto error;

	/* ref tmpfs inode */
	get_inode(walk.target);

	plog_track_inode(walk.target, path);

	/**
	 * Set Output
	 */
	*vnode_id = (ino_t)walk.target;
	*vnode_type = walk.target->type == FS_REG ? FS_NODE_REG : FS_NODE_DIR;
	*vnode_size = walk.target->size;
	*vnode_private = walk.target;

	path_fini(&walk.path_record);
	return 0;
error:
	path_fini(&walk.path_record);
	if (err == -EAGAIN) {
		// TODO: For symlink handling
		// fr->next_fs_idx = walk.next_fs_idx;
		// fr->path_advanced = walk.path_advanced;
		// err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
		// /* FIXME(MK): We should use new error codes for IPC. */
		// if (err)
		// 	return -ENOSPC;
		// return -EAGAIN;
		BUG_ON(1); /* TODO: implement symlink */
	} else if (err == -E_NAMEX_NOENT) {
		err = -ENOENT;
	}
	return err;
}

ssize_t tmpfs_read(void *operator, off_t offset, size_t size, char *buf)
{
	struct inode *inode = (struct inode *)operator;
	ssize_t ret = 0;
	if (inode)
		ret = tfs_file_read(inode, offset, buf, size);
	return ret;
}

ssize_t tmpfs_write(void *operator, off_t offset, size_t size, const char *buf)
{
	struct inode *inode = (struct inode *)operator;
	ssize_t ret = 0;
	if (inode) {
		const char *path = plog_get_inode_path(inode);
		if (path && !plog_is_replaying() &&
		    plog_append_write(path, offset, buf, size) < 0)
			return -ENOSPC;
		ret = tfs_file_write(inode, offset, buf, size);
	}
	return ret;
}

int tmpfs_close(void *operator, bool is_dir)
{
	int ret;

	ret = put_inode((struct inode *)operator);
	if (ret) {
		fs_debug_error("put_inode failed %d\n", ret);
		return ret;
	}

	return 0;
}

int __fs_creat(const char *path, mode_t mode)
{
	struct inode *dirat = NULL;
	struct path path_record;
	const char *leaf = path;
	int err;

	BUG_ON(!path);
	BUG_ON(*path != '/');
	WARN_ON(mode, "mode is ignored by fs_creat");

	path_init(&path_record);
	path_append(&path_record, tmpfs_root_dent);

	/* Skip leading slashes — path_record is already rooted, tfs_namex
	 * does not accept a leading '/' (it walks relative components). */
	while (*leaf == '/')
		leaf++;

	if (!*leaf) {
		/* Path was just "/" — target is the root itself */
		err = -EISDIR;
		goto error;
	}
	err = tfs_namex(&leaf, &path_record,
			/* mkdir_p */ 0,
			/* follow_symlink */ 1);
	if (err)
		goto error;

	dirat = path_last_inode(&path_record);
	if (!plog_is_replaying() && plog_append_creat(path, mode) < 0) {
		err = -ENOSPC;
		goto error;
	}
	err = tfs_creat(dirat, leaf, strlen(leaf),
			mode, NULL);
	if (err)
		goto error;

	// FIXME(TCZ): add fd entry to a process' open file table and return fd
	// TODO(TCZ): change to the following call as per POSIX
	// open (filename, O_WRONLY | O_CREAT | O_TRUNC, mode)
error:
	path_fini(&path_record);
	return err;
}

int tmpfs_creat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_creat(fr->creat.pathname, fr->creat.mode);
}

int __tmpfs_unlink(enum fs_req_type req, const char *path, int flags)
{
	/* NOTE(HYQ): relative path should be translated outside */
	int dirfd = AT_FDROOT;
	struct tmpfs_walk_path walk;
	int err;

	if (flags & (~AT_SYMLINK_NOFOLLOW)) {
		/* flags contain invalid flag bits. */
		return -EINVAL;
	}

	walk.path = path;
	walk.path_len = strlen(path);
	walk.not_found_callback = NULL;

	err = handle_xxxat(dirfd, &walk);
	if (err)
		goto error;

	/* Found the target */
	/* walk->parent is the parent, walk->target is the target file inode. */
	if (req == FS_REQ_UNLINK && walk.target->type == FS_DIR) {
		err = -EISDIR;
		goto error;
	} else if (req == FS_REQ_RMDIR && walk.target->type != FS_DIR) {
		err = -ENOTDIR;
		goto error;
	}
	if (!plog_is_replaying() &&
	    plog_append_unlink(path, req == FS_REQ_RMDIR) < 0) {
		err = -ENOSPC;
		goto error;
	}
	err = cxlfs_unlink_node(path_last_inode(&walk.path_record)->disk_ino,
				walk.leaf, strlen(walk.leaf));
	if (err)
		goto error;

	err = tfs_remove(path_last_inode(&walk.path_record),
			 walk.leaf, strlen(walk.leaf));
	if (err)
		goto error;

	err = 0;

error:

	path_fini(&walk.path_record);
	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	return err;
}

int tmpfs_unlink(const char *path, int flags)
{
	return __tmpfs_unlink(FS_REQ_UNLINK, path, flags);
}

int tmpfs_rmdir(const char *path, int flags)
{
	return __tmpfs_unlink(FS_REQ_RMDIR, path, flags);
}

int tmpfs_mkdir(const char *path, mode_t mode)
{
	struct tmpfs_walk_path walk;
	struct inode *parent;
	int err;

	walk.path = path;
	walk.path_len = strlen(path);
	walk.not_found_callback = NULL;

	debug("path=%s\n", path);

	err = handle_xxxat(AT_FDROOT, &walk);

	if (!err) {
		/* dentry exists */
		err = -EEXIST;
		goto error;
	}

	if (err != -ENOENT)
		goto error;

	parent = path_last_inode(&walk.path_record);
	debug("parent=%p leaf=%s\n", parent, walk.leaf);
	/**
	 * Found the target.
	 * The last component in path is the parent,
	 * walk->target is the target file inode.
	 */

	if (!plog_is_replaying() && plog_append_mkdir(path, mode) < 0) {
		err = -ENOSPC;
		goto error;
	}
	err = tfs_mkdir(parent, walk.leaf, strlen(walk.leaf), mode, NULL);
	if (err)
		goto error;
	err = 0;

error:
	path_fini(&walk.path_record);

	debug("err=%d\n", err);
	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	if (err == -E_NAMEX_NOENT)
		err = -ENOENT;
	return err;
}

int tmpfs_rename(const char *oldpath, const char *newpath)
{
	struct tmpfs_walk_path walk_old, walk_new;
	struct inode *inode_to_remove = NULL;
	struct dentry *dent_old, *dent_new /* of new path */, *tmp_dent;
	int new_exists, err;

	walk_old.path = oldpath;
	walk_old.path_len = strlen(oldpath);
	walk_old.not_found_callback = NULL;
	walk_old.follow_symlink = 0;
	if ((err = handle_xxxat(AT_FDROOT, &walk_old))) {
		if (err == -E_NAMEX_NOENT)
			err = -ENOENT;
		return err;
	}
	dent_old = *walk_old.leaf == '\0' ?
		path_last_dent(&walk_old.path_record) : walk_old.target_dent;
	u64 old_parent_ino = path_last_inode(&walk_old.path_record)->disk_ino;
	const char *old_leaf = walk_old.leaf;

	/* check . and .. in the final component */
	if (!strcmp(".", walk_old.leaf) || !strcmp("..", walk_old.leaf))
		return -EINVAL;

	/* FIXME(MK): Shall we move this forward? */
	path_fini(&walk_old.path_record);

	walk_new.path = newpath;
	walk_new.path_len = strlen(newpath);
	walk_new.not_found_callback = NULL;
	walk_new.follow_symlink = 0;
	err = handle_xxxat(AT_FDROOT, &walk_new);
	if (err && err != -ENOENT) {
		path_fini(&walk_new.path_record);
		if (err == -E_NAMEX_NOENT)
			err = -ENOENT;
		return err;
	}
	new_exists = err != -ENOENT;
	u64 new_parent_ino = path_last_inode(&walk_new.path_record)->disk_ino;
	const char *new_leaf = walk_new.leaf;

	/* check old is not a ancestor of new */
	/* TODO(HYQ): when touch symlink? */
	if (strncmp(walk_old.path, walk_new.path, strlen(walk_old.path)) == 0) {
		if (walk_new.path[strlen(walk_old.path)] == '/')
			return -EINVAL;
	}
	if (new_exists) {
		dent_new = *walk_new.leaf == '\0' ?
			path_last_dent(&walk_new.path_record)
			: walk_new.target_dent;

		/* FIXME(MK): Shall we move this forward? */
		path_fini(&walk_new.path_record);

		// same dentry or inode, then do nothing
		if (dent_old == dent_new ||
		    dent_old->inode == dent_new->inode) {
			return 0;
		}

		// if old is dir, new should be dir;
		// if old is not a dir, new should not be a dir
		if ((dent_old->inode->type == FS_DIR &&
		     dent_new->inode->type != FS_DIR)) {
			return -ENOTDIR;
		}
		if ((dent_old->inode->type != FS_DIR &&
		     dent_new->inode->type == FS_DIR)) {
			return -EISDIR;
		}

		// if new is dir, it should be empty
		if (dent_new->inode->type == FS_DIR &&
		    !htable_empty(&dent_new->inode->dentries))
			return -ENOTEMPTY;
		if (!plog_is_replaying() &&
		    plog_append_rename(oldpath, newpath) < 0)
			return -ENOSPC;
		err = cxlfs_rename_node(old_parent_ino, old_leaf, strlen(old_leaf),
					new_parent_ino, new_leaf, strlen(new_leaf));
		if (err)
			return err;

		inode_to_remove = dent_new->inode;
		/* The destination dentry acquires its own reference before the old
		 * source dentry drops its reference below. */
		dent_new->inode = get_inode(dent_old->inode);
	} else {
		if (!plog_is_replaying() &&
		    plog_append_rename(oldpath, newpath) < 0)
			return -ENOSPC;
		err = cxlfs_rename_node(old_parent_ino, old_leaf, strlen(old_leaf),
					new_parent_ino, new_leaf, strlen(new_leaf));
		if (err)
			return err;
		tmp_dent = new_dent(dent_old->inode, walk_new.leaf,
				    strlen(walk_new.leaf));
		if (CHCORE_IS_ERR(tmp_dent)) {
			return CHCORE_PTR_ERR(tmp_dent);
		}

		htable_add(&path_last_inode(&walk_new.path_record)->dentries,
			   (u32)(tmp_dent->name.hash), &tmp_dent->node);
		path_fini(&walk_new.path_record);
	}

	if (inode_to_remove) {
		if ((err = put_inode(inode_to_remove))) {
			return err;
		}
	}

	/* Save inode pointer before freeing the old dentry. */
	struct inode *moved_inode = dent_old->inode;

	if ((err = put_inode(dent_old->inode))) {
		return err;
	}
	// remove dentry from parent
	htable_del(&dent_old->node);
	// free dentry
	free(dent_old);

	plog_track_inode(moved_inode, newpath);

	return 0;
}

static int __dirent_filler(void **dirpp, void *end, char *name, off_t off,
			 unsigned char type, ino_t ino)
{
	struct dirent *dirp = *(struct dirent **)dirpp;
	void *p = dirp;
	unsigned short len = strlen(name) + 1 + sizeof(dirp->d_ino) +
			     sizeof(dirp->d_off) + sizeof(dirp->d_reclen) +
			     sizeof(dirp->d_type);
	p += len;
	if (p > end)
		return -EAGAIN;
	dirp->d_ino = ino;
	dirp->d_off = off;
	dirp->d_reclen = len;
	dirp->d_type = type;
	strlcpy(dirp->d_name, name, DIRENT_NAME_MAX);
	*dirpp = p;
	return len;
}

static int __tfs_scan(struct inode *dir, unsigned int start, void *buf, void *end,
		    int *read_bytes)
{
	int cnt = 0;
	int b;
	int ret;
	ino_t ino;
	void *p = buf;
	unsigned char type;
	struct dentry *iter;

	for_each_in_htable(iter, b, node, &dir->dentries)
	{
		if (cnt >= start) {
			/* TODO(YJF): use the correct ino and type */
			type = iter->inode->type;
			ino = iter->inode->size;
			ret = __dirent_filler(&p, end, iter->name.str, cnt, type,
					    ino);
			if (ret <= 0) {
				if (read_bytes)
					*read_bytes = (int)(p - buf);
				return (int)(cnt - start);
			}
		}
		cnt++;
	}
	if (read_bytes)
		*read_bytes = (int)(p - buf);
	return (int)(cnt - start);
}

int __fs_getdents(int fd, unsigned int count, ipc_msg_t *ipc_msg)
{
	struct inode *inode = (struct inode *)server_entrys[fd]->vnode->private;
	int ret = 0, read_bytes;
	if (inode) {
		if (inode->type == FS_DIR) {
			ret = __tfs_scan(inode, server_entrys[fd]->offset,
				       ipc_get_msg_data(ipc_msg),
				       ipc_get_msg_data(ipc_msg) + count,
				       &read_bytes);
			server_entrys[fd]->offset += ret;
			ret = read_bytes;
		} else
			ret = -ENOTDIR;
	} else
		ret = -ENOENT;
	return ret;
}

int tmpfs_getdents(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_getdents(fr->getdents64.fd, fr->getdents64.count, ipc_msg);
}

int tmpfs_ftruncate(void *operator, size_t len)
{
	struct inode *inode;
	inode = (struct inode *)operator;

	/* The named file is a directory */
	if (inode->type == FS_DIR)
		return -EISDIR;
	/* fd does not reference a regular file */
	if (inode->type != FS_REG)
		return -EINVAL;

	const char *path = plog_get_inode_path(inode);
	if (path && !plog_is_replaying() &&
	    plog_append_truncate(path, len) < 0)
		return -ENOSPC;
	return tfs_truncate(inode, len);
}

static void __fill_statbuf(struct stat *statbuf, struct inode *inode)
{
	assert(inode);
	statbuf->st_dev = 0;
	/* TODO(MK): We should use a UUID for inode. */
	statbuf->st_ino = (ino_t)inode;

	if (inode->type == FS_REG)
		statbuf->st_mode = S_IFREG;
	else if (inode->type == FS_DIR)
		statbuf->st_mode = S_IFDIR;
	else {
		assert(0);
	}
	statbuf->st_nlink = inode->nlinks;
	/* TODO(MK): uid/gid. */
	statbuf->st_uid = 0;
	statbuf->st_gid = 0;

	statbuf->st_rdev = 0;
	BUG_ON(inode->size > LONG_MAX);
	statbuf->st_size = (off_t)inode->size;

	/* TODO(MK): Use real xtime. */
	statbuf->st_atime = 0;
	statbuf->st_mtime = 0;
	statbuf->st_ctime = 0;

	/* TODO(MK): Handle the following two. */
	statbuf->st_blksize = 0;
	statbuf->st_blocks = 0;
}

int __fs_fstatat(const char *path, struct stat *st, int flags)
{
	// (int dirfd, const char *path, struct stat *stat, int flags)
	/* NOTE(HYQ): path should be absolute path, convert relative path outside */
	int dirfd = AT_FDROOT;
	struct tmpfs_walk_path walk;
	int err;

	if (flags & (~AT_SYMLINK_NOFOLLOW)) {
		/* flags contain invalid flag bits. */
		return -EINVAL;
	}

	walk.path = path;
	walk.path_len = strlen(path);
	walk.not_found_callback = NULL;

	err = handle_xxxat(dirfd, &walk);
	if (err)
		goto error;

	/* Found the target */
	/* walk->parent is the parent, walk->target is the target file inode. */
	__fill_statbuf(st, walk.target);

	err = 0;

error:

	path_fini(&walk.path_record);
	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	if (err == -E_NAMEX_NOENT) {
		err = -ENOENT;
	}
	return err;
}

int tmpfs_fstatat(const char *path, struct stat *st, int flags)
{
	return __fs_fstatat(path, st, flags);
}

int __fs_fstat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	struct inode *inode;
	// int err;

	int fd = fr->stat.fd;
	struct stat *stat = (struct stat *)ipc_get_msg_data(ipc_msg);

	assert(server_entrys[fd]);
	inode = (struct inode *)server_entrys[fd]->vnode->private;
	__fill_statbuf(stat, inode);

	// err = ipc_set_msg_data(ipc_msg, stat, 0, sizeof(*stat));
	// /* FIXME(MK): We should use new error codes for IPC. */
	// if (err)
	// 	return -ENOSPC;
	return 0;
}

int tmpfs_fstat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_fstat(ipc_msg, fr);
}

static void __fill_statfsbuf(struct statfs *statbuf)
{
	const int faked_large_value = 1024 * 1024 * 512;
	/* Type of filesystem (see below): ChCorE7mpf5 */
	statbuf->f_type = 0xCCE7A9F5;
	/* Optimal transfer block size */
	statbuf->f_bsize = PAGE_SIZE;
	/* Total data blocks in filesystem */
	statbuf->f_blocks = faked_large_value;
	/* Free blocks in filesystem */
	statbuf->f_bfree = faked_large_value;
	/* Free blocks available to unprivileged user */
	statbuf->f_bavail = faked_large_value;
	/* Total file nodes in filesystem */
	statbuf->f_files = faked_large_value;
	/* Free file nodes in filesystem */
	statbuf->f_ffree = faked_large_value;
	/* Filesystem ID (See man page) */
	memset(&statbuf->f_fsid, 0, sizeof(statbuf->f_fsid));
	/* Maximum length of filenames */
	statbuf->f_namelen = MAX_FILENAME_LEN;
	/* Fragment size (since Linux 2.6) */
	statbuf->f_frsize = 512; /* XXX(MK): the value? */
	/* Mount flags of filesystem (since Linux 2.6.36) */
	statbuf->f_flags = 0;
}

int __fs_fstatfs(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	// struct statfs stat;
	// int err;

	int fd = fr->stat.fd;
	struct statfs *stat = (struct statfs *)ipc_get_msg_data(ipc_msg);

	/* TODO(MK): Check fd validity. */
	assert(server_entrys[fd]);
	__fill_statfsbuf(stat);

	// err = ipc_set_msg_data(ipc_msg, stat, 0, sizeof(stat));
	// /* FIXME(MK): We should use new error codes for IPC. */
	// if (err)
	// 	return -ENOSPC;
	return 0;
}

int __fs_fstatfsat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	// (int dirfd, const char *path, struct stat *stat)
	int dirfd = fr->stat.dirfd;
	const char *path = fr->stat.pathname;
	struct statfs *stat = (struct statfs *)ipc_get_msg_data(ipc_msg);
	struct tmpfs_walk_path walk;
	int err;

	walk.path = path;
	walk.path_len = strlen(path);
	walk.not_found_callback = NULL;

	err = handle_xxxat(dirfd, &walk);
	if (err)
		goto error;

	/* Found the target */
	/* walk->parent is the parent, walk->target is the target file inode. */
	__fill_statfsbuf(stat);

	// err = ipc_set_msg_data(ipc_msg, &stat, 0, sizeof(stat));
	/* FIXME(MK): We should use new error codes for IPC. */
	// if (err)
	// 	return -ENOSPC;
	return 0;
error:
	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	return err;
}

int tmpfs_statfs(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_fstatfsat(ipc_msg, fr);
}

int tmpfs_fstatfs(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_fstatfs(ipc_msg, fr);
}

int __fs_faccessat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	// (int dirfd, const char *path, struct stat *stat)
	// int dirfd = fr->faccessat;
	const char *path = fr->faccessat.pathname;
	mode_t mode = fr->faccessat.mode;
	int flags = fr->faccessat.flags;
	struct tmpfs_walk_path walk;
	int err;

	if (flags & (~AT_EACCESS)) {
		/* flags contain invalid flag bits. */
		return -EINVAL;
	}

	walk.path = path;
	walk.path_len = strlen(path);
	walk.not_found_callback = NULL;

	err = handle_xxxat(AT_FDROOT, &walk);
	if (err)
		goto error;

	/* Found the target */
	/* walk->parent is the parent, walk->target is the target file inode. */
	// TODO(TCZ): check walk->target->mode with uid & gid
	// Note: F_OK requires no additional check, since handle_xxxat can
	// return -ENOENT when file is not found
	if (mode & R_OK) {
	}
	if (mode & W_OK) {
	}
	if (mode & X_OK) {
	}
	error("fs_faccessat always return 0 when the file exists\n");

	return 0;
error:
	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	return err;
}

int tmpfs_faccessat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_faccessat(ipc_msg, fr);
}

int __fs_symlinkat_callback(int retcode, struct tmpfs_walk_path *walk, void *data)
{
	return tfs_mknod(path_last_inode(&walk->path_record),
			 walk->leaf, strlen(walk->leaf), FS_SYM,
			 0777, data, &(walk->target_dent));
}

int __fs_symlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	char *target = fr->symlinkat.target;
	char *linkpath = fr->symlinkat.linkpath;
	struct tmpfs_walk_path walk;
	int err;

	walk.path = linkpath;
	walk.path_len = strlen(linkpath);
	walk.not_found_callback = __fs_symlinkat_callback;
	walk.callback_data = target;

	err = handle_xxxat(AT_FDROOT, &walk);

	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	return err;
}

int tmpfs_symlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_symlinkat(ipc_msg, fr);
}

ssize_t __fs_readlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	const char *path = fr->readlinkat.pathname;
	struct tmpfs_walk_path walk;
	ssize_t size;
	int err;

	walk.path = path;
	walk.path_len = strlen(path);
	walk.follow_symlink = 0;

	err = handle_xxxat(AT_FDROOT, &walk);
	if (err)
		goto error;

	if (walk.target->type != FS_SYM) {
		err = -EINVAL;
		goto error;
	}

	size = tfs_file_read(walk.target, 0, fr->readlinkat.buf, FS_REQ_PATH_LEN);
	if (size < 0)
		goto error;

	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	/* FIXME(MK): We should use new error codes for IPC. */
	if (err)
		goto error;

	return size;

error:
	// if (err == -EAGAIN) {
	// 	fr->next_fs_idx = walk.next_fs_idx;
	// 	fr->path_advanced = walk.path_advanced;
	// 	err = ipc_set_msg_data(ipc_msg, fr, 0, sizeof(*fr));
	// 	/* FIXME(MK): We should use new error codes for IPC. */
	// 	if (err)
	// 		return -ENOSPC;
	// 	return -EAGAIN;
	// }
	return err;
}

ssize_t tmpfs_readlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_readlinkat(ipc_msg, fr);
}

static int __fs_punch_hole(struct inode *inode, off_t offset, off_t len)
{
	u64 page_no, page_off;
	u64 cur_off = offset;
	off_t to_remove;
	void *page;
	int err;

	while (len > 0) {
		page_no = cur_off / PAGE_SIZE;
		page_off = cur_off % PAGE_SIZE;

		to_remove = MIN(len, PAGE_SIZE - page_off);
		cur_off += to_remove;
		len -= to_remove;
		page = radix_get(&inode->data, page_no);
		if (page) {
			if (to_remove == PAGE_SIZE || cur_off == inode->size) {
				err = radix_del(&inode->data, page_no, 1);
				return err;
			}
			else
				memset(page + page_off, 0, to_remove);
		}
	}
	return 0;
}

static int __fs_collapse_range(struct inode *inode, off_t offset, off_t len)
{
	u64 page_no1, page_no2;
	u64 cur_off = offset;
	void *page1;
	void *page2;
	u64 remain;
	int err;
	off_t dist;

	/* To ensure efficient implementation, offset and len must be a mutiple
	 * of the filesystem logical block size */
	if (offset % PAGE_SIZE || len % PAGE_SIZE)
		return -EINVAL;
	if (offset + len >= inode->size)
		return -EINVAL;

	remain = ((inode->size + PAGE_SIZE - 1) - (offset + len)) / PAGE_SIZE;
	dist = len / PAGE_SIZE;
	while (remain-- > 0) {
		page_no1 = cur_off / PAGE_SIZE;
		page_no2 = page_no1 + dist;

		cur_off += PAGE_SIZE;
		page1 = radix_get(&inode->data, page_no1);
		page2 = radix_get(&inode->data, page_no2);
		if (page1) {
			err = radix_del(&inode->data, page_no1, 1);
			if (err)
				goto error;
		}
		if (page2) {
			err = radix_add(&inode->data, page_no1, page2);
			if (err)
				goto error;
			err = radix_del(&inode->data, page_no2, 0);
			if (err)
				goto error;
		}
	}

	inode->size -= len;
	return 0;

error:
	error("Error in collapse range!\n");
	return err;
}

static int __fs_zero_range(struct inode *inode, off_t offset, off_t len, mode_t mode)
{
	u64 page_no, page_off;
	u64 cur_off = offset;
	off_t to_zero;
	void *page;

	while (len > 0) {
		page_no = cur_off / PAGE_SIZE;
		page_off = cur_off % PAGE_SIZE;

		to_zero = MIN(len, PAGE_SIZE - page_off);
		cur_off += to_zero;
		len -= to_zero;
		if (!len)
			to_zero = PAGE_SIZE;
		page = radix_get(&inode->data, page_no);
		if (!page) {
			page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
			if (!page)
				return -ENOSPC;
			radix_add(&inode->data, page_no, page);
		}
		BUG_ON(!page);
		memset(page + page_off, 0, to_zero);
	}

	if ((!(mode & FALLOC_FL_KEEP_SIZE)) && (offset + len > inode->size))
		inode->size = offset + len;

	return 0;
}

static int __fs_insert_range(struct inode *inode, off_t offset, off_t len)
{
	u64 page_no1, page_no2;
	void *page;
	int err;
	off_t dist;

	/* To ensure efficient implementation, this mode has the same
	 * limitations as FALLOC_FL_COLLAPSE_RANGE regarding the granularity of
	 * the operation. (offset and len must be a mutiple of the filesystem
	 * logical block size) */
	if (offset % PAGE_SIZE || len % PAGE_SIZE)
		return -EINVAL;
	/* If the offset is equal to or greater than the EOF, an error is
	 * returned. For such operations, ftruncate should be used. */
	if (offset >= inode->size)
		return -EINVAL;

	page_no1 = (inode->size + PAGE_SIZE - 1) / PAGE_SIZE;
	dist = len / PAGE_SIZE;
	while (page_no1 >= offset / PAGE_SIZE) {
		page_no2 = page_no1 + dist;
		BUG_ON(radix_get(&inode->data, page_no2));
		page = radix_get(&inode->data, page_no1);
		if (page) {
			err = radix_del(&inode->data, page_no1, 0);
			if (err)
				goto error;
			err = radix_add(&inode->data, page_no2, page);
			if (err)
				goto error;
		}
		page_no1--;
	}

	inode->size += len;
	return 0;

error:
	error("Error in insert range!\n");
	return err;
}

int __fs_fallocate(struct fs_request *fr)
{
	int fd = fr->fallocate.fd;
	mode_t mode = fr->fallocate.mode;
	off_t offset = fr->fallocate.offset;
	off_t len = fr->fallocate.len;
	int keep_size;
	struct inode *inode;

	inode = (struct inode *)server_entrys[fd]->vnode->private;

	if (offset < 0 || len <= 0)
		return -EINVAL;

	/* return error if mode is not supported */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE |
		     FALLOC_FL_INSERT_RANGE))
		return -EOPNOTSUPP;
	/* These transforms still operate on the old volatile radix layout. */
	if (inode->disk_ino &&
	    (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_COLLAPSE_RANGE |
		     FALLOC_FL_ZERO_RANGE | FALLOC_FL_INSERT_RANGE)))
		return -EOPNOTSUPP;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		return __fs_punch_hole(inode, offset, len);
	if (mode & FALLOC_FL_COLLAPSE_RANGE)
		return __fs_collapse_range(inode, offset, len);
	if (mode & FALLOC_FL_ZERO_RANGE)
		return __fs_zero_range(inode, offset, len, mode);
	if (mode & FALLOC_FL_INSERT_RANGE)
		return __fs_insert_range(inode, offset, len);

	keep_size = mode & FALLOC_FL_KEEP_SIZE ? 1 : 0;
	return tfs_allocate(inode, offset, len, keep_size);
}

int tmpfs_fallocate(ipc_msg_t *ipc_msg, struct fs_request *fr)
{
	return __fs_fallocate(fr);
}

int __fs_fcntl(void *operator, int fd, int fcntl_cmd, int fcntl_arg)
{
	struct server_entry *entry;
	int ret = 0;

	if ((entry = server_entrys[fd]) == NULL)
		return -EBADF;

	switch (fcntl_cmd) {
	case F_GETFL:
		ret = entry->flags;
		break;
	case F_SETFL: {
		// file access mode and the file creation flags
		// should be ingored, per POSIX
		int effective_bits = (~O_ACCMODE & ~O_CLOEXEC & ~O_CREAT &
				      ~O_DIRECTORY & ~O_EXCL & ~O_NOCTTY &
				      ~O_NOFOLLOW & ~O_TRUNC & ~O_TTY_INIT);

		entry->flags = (fcntl_arg & effective_bits) |
			     (entry->flags & ~effective_bits);
		break;
	}
	case F_DUPFD:
		break;
	case F_GETOWN:
	case F_SETOWN:
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
	default:
		error("unsopported fcntl cmd\n");
		ret = -1;
		break;
	}

	return ret;
}

int tmpfs_fcntl(void *operator, int fd, int fcntl_cmd, int fcntl_arg)
{
	return __fs_fcntl(operator, fd, fcntl_cmd, fcntl_arg);
}

#ifdef CHCORE_ENABLE_FMAP
vaddr_t tmpfs_get_page_addr(void *operator, size_t offset)
{
	struct inode *inode;
	void *page;
	size_t page_no;

	inode = (struct inode *)operator;
	page_no = offset / PAGE_SIZE;
	page = radix_get(&inode->data, page_no);
	if (!page && inode->disk_ino) {
		/* fmap requires a normal tmpfs page that user_fault_map can copy or
		 * map.  The CXL block remains authoritative; this DRAM page is only
		 * a disposable projection rebuilt after recovery. */
		page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
		if (!page)
			return 0;
		memset(page, 0, PAGE_SIZE);
		ssize_t nr = cxlfs_read(inode->disk_ino, page_no * PAGE_SIZE,
					 page, PAGE_SIZE);
		if (nr < 0 || radix_add(&inode->data, page_no, page)) {
			free(page);
			return 0;
		}
	}

	return (vaddr_t)page;
}
#endif

static int tmpfs_fsync(void)
{
	/*
	 * Publish the stable CXLFS state, but retain the redo tail across fsync.
	 * plog_append_raw() checkpoints and truncates only when the 4 MiB log is
	 * full; initialization/recovery may also establish a new empty epoch.
	 */
	return plog_checkpoint();
}

static int tmpfs_get_status(struct fs_server_status *status)
{
	memset(status, 0, sizeof(*status));
	status->magic = FS_SERVER_STATUS_MAGIC;
	status->backend = FS_BACKEND_CXLFS;
	status->mounted = cxlfs_is_mounted();
	status->data_valid = cxlfs_boot_data_validated;
	status->owner_machine = cxlfs_machine();
	status->fresh = cxlfs_mount_was_fresh;
	status->generation = cxlfs_generation();
	status->root_ino = cxlfs_root_ino();
	status->data_checksum = cxlfs_boot_data_checksum;
	return status->mounted && status->data_valid && status->generation &&
	       status->root_ino && status->data_checksum ? 0 : -EIO;
}

struct fs_server_ops server_ops = {
	.mount = default_server_operation,
	.umount = default_server_operation,
	.open = tmpfs_open,
	.read = tmpfs_read,
	.write = tmpfs_write,
	.close = tmpfs_close,
	.creat = tmpfs_creat,
	.unlink = tmpfs_unlink,
	.mkdir = tmpfs_mkdir,
	.rmdir = tmpfs_rmdir,
	.rename = tmpfs_rename,
	.getdents64 = tmpfs_getdents,
	.ftruncate = tmpfs_ftruncate,
	.fstatat = tmpfs_fstatat,
	.fstat = tmpfs_fstat,
	.statfs = tmpfs_statfs,
	.fstatfs = tmpfs_fstatfs,
	.faccessat = tmpfs_faccessat,
	.symlinkat = tmpfs_symlinkat,
	.readlinkat = tmpfs_readlinkat,
	.fallocate = tmpfs_fallocate,
	.fcntl = tmpfs_fcntl,
	.fsync = tmpfs_fsync,
	.get_status = tmpfs_get_status,
#ifdef CHCORE_ENABLE_FMAP
	.fmap_get_page_addr = tmpfs_get_page_addr,
#endif
};
