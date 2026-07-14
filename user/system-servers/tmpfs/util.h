#pragma once

#include <assert.h>
#include <chcore/container/hashtable.h>
#include <chcore/container/radix.h>
#include <chcore/cpio.h>
#include <chcore/defs.h>
#include <chcore-internal/fs_defs.h>
#include <chcore/ipc.h>
#include <chcore/launch_kern.h>
#include <chcore/syscall.h>
#include <chcore/type.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>

#include "defs.h"

struct tmpfs_walk_path;
typedef int (*tmpfs_walk_cb_t)(int retcode, struct tmpfs_walk_path *walk,
			       void *data);

struct tmpfs_walk_path {
	const char *path;
	size_t path_len;

	struct path path_record;

	int follow_symlink;

	struct inode *target; /* The leaf file if found. */
	struct dentry *target_dent;
	const char *leaf;
	// used when the path points to a symbolic link
	char symlink_buf[FS_REQ_PATH_LEN + 1];
	int cursor;

	tmpfs_walk_cb_t not_found_callback;
	void *callback_data;

	/* For walking to the next fs (EAGAIN). */
	int next_fid;
	int next_fs_idx;
	size_t path_advanced;
};

/**
 * function declarations
 */
static inline u64 hash_chars(const char *str, ssize_t len);
static inline u64 hash_string(struct string *s);
static inline int init_string(struct string *s, const char *name, size_t len);

static inline int new_fid_record(struct inode *inode);
static inline struct fid_record *get_fid_record(int fid);

struct inode *new_inode(void);
int del_inode(struct inode *inode);
struct inode *new_dir(struct inode *);
struct inode *new_reg(void);
struct inode *new_symlink(const char *symlink);
struct dentry *new_dent(struct inode *inode, const char *name, size_t len);
int del_dent(struct dentry *dent);


int tfs_mknod(struct inode *dir, const char *name, size_t len, u64 file_type,
	      mode_t mode, const char *symlink, struct dentry **dent);
int tfs_creat(struct inode *dir, const char *name, size_t len, mode_t mode,
	      struct dentry **dent);
int tfs_mkdir(struct inode *dir, const char *name, size_t len, mode_t mode,
	      struct dentry **dent);
int tfs_attach_disk_node(struct inode *dir, const char *name, size_t len,
			 u64 disk_ino, u64 type, mode_t mode, size_t size,
			 struct inode **inode_out);

int tfs_truncate(struct inode *inode, size_t size);
int tfs_allocate(struct inode *inode, off_t offset, off_t len, int keep_size);
int tfs_unref_or_remove_inode(struct inode *inode);
int tfs_remove(struct inode *dir, const char *name, size_t len);
ssize_t tfs_file_write(struct inode *inode, off_t offset, const char *data,
		       size_t size);
ssize_t tfs_file_read(struct inode *inode, off_t offset, char *buff,
		      size_t size);

int tfs_namex(const char **name, struct path *path,
	      int mkdir_p, int follow_symlink);
struct dentry *tfs_lookup(struct inode *dir, const char *name, size_t len);

int handle_xxxat(int dirfd, struct tmpfs_walk_path *walk);

/**
 * struct path related functions.
 */
int path_init(struct path *path);
int path_append(struct path *path, struct dentry *dentry);
int path_goback(struct path *path);
struct dentry *path_last_dent(struct path *path);
struct inode *path_last_inode(struct path *path);
struct inode *path_last_dir_inode(struct path *path);
int path_to_str(struct path *path, char *buf, int length, int expand_symlink);
int path_copy(struct path *dst, struct path *src);
int path_fini(struct path *path);
void path_enter_symlink(struct path *path);
void path_exit_symlink(struct path *path);
int path_in_symlink(struct path *path);


/**
 * inline functions
 */
static inline u64 hash_chars(const char *str, ssize_t len)
{
	u64 seed = 131; /* 31 131 1313 13131 131313 etc.. */
	u64 hash = 0;
	int i;

	if (len < 0) {
		while (*str) {
			hash = (hash * seed) + *str;
			str++;
		}
	} else {
		for (i = 0; i < len; ++i)
			hash = (hash * seed) + str[i];
	}

	return hash;
}

/* BKDR hash */
static inline u64 hash_string(struct string *s)
{
	return (s->hash = hash_chars(s->str, s->len));
}

static inline int init_string(struct string *s, const char *name, size_t len)
{
	int i;

	s->str = malloc(len + 1);
	if (!s->str)
		return -ENOMEM;
	s->len = len;

	for (i = 0; i < len; ++i)
		s->str[i] = name[i];
	s->str[len] = '\0';

	hash_string(s);
	return 0;
}

static inline struct inode *get_inode(struct inode *i)
{
	i->refcnt++;
	return i;
}
static inline int put_inode(struct inode *i) {
	i->refcnt--;
	assert(i->refcnt >= 0);
	if (!i->refcnt)
		return del_inode(i);
	return 0;
}

static inline int new_fid_record(struct inode *inode)
{
	int id;

	id = alloc_id(&fidman);
	/* 0 is already allocated during init. */
	assert(id > 0);

	fid_records[id].inode = inode;
	get_inode(inode);

	return id;
}

static inline struct fid_record *get_fid_record(int fid)
{
	int err;

	if (fid == 0)
		return NULL;

	err = query_id(&fidman, fid);
	if (err)
		/* Not found. */
		return NULL;

	return &fid_records[fid];
}

static inline int del_fid_record(int fid)
{
	int err;

	/* We don't use 0 for fd. */
	if (fid == 0)
		return -EBADF;

	err = query_id(&fidman, fid);
	if (err)
		/* Not found. */
		return -EBADF;

	put_inode(fid_records[fid].inode);

	fid_records[fid].inode = NULL;
	fid_records[fid].flags = 0;
	fid_records[fid].offset = 0;

	err = free_id(&fidman, fid);
	assert(err == 0);

	return 0;
}

static inline struct dentry *get_dent(struct dentry *d)
{
	d->refcnt++;
	return d;
}
static inline int put_dent(struct dentry *d)
{
	d->refcnt--;
	assert(d->refcnt >= 0);
	if (!d->refcnt)
		return del_dent(d);
	return 0;
}
