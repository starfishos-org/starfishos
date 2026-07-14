#pragma once

#include <chcore/container/hashtable.h>
#include <chcore/type.h>
#include <malloc.h>
#include <sys/types.h>

#include "defs.h"
#include "util.h"

struct openat_callback_args {
	int flags;
	mode_t mode;
	const char *path;
};

struct __fmap_radix_scan_cb_args {
	vaddr_t *buffer;
	size_t capacity; /* Buffer capacity in entries. */
	size_t nr_filled;
};

extern struct inode *tmpfs_root;
extern struct dentry *tmpfs_root_dent;
extern struct id_manager fidman;
extern struct fid_record fid_records[MAX_NR_FID_RECORDS];
extern int cxlfs_boot_data_validated;
extern int cxlfs_mount_was_fresh;
extern u64 cxlfs_boot_data_checksum;

int tmpfs_open(char *path, int flags, int mode, ino_t *vnode_id,
	       size_t *vnode_size, int *vnode_type, void **vnode_private);
ssize_t tmpfs_read(void *operator, off_t offset, size_t size, char *buf);
int tmpfs_close(void *operator, bool is_dir);

int __fs_creat(const char *path, mode_t mode);
int tmpfs_mkdir(const char *path, mode_t mode);
int tmpfs_unlink(const char *path, int flags);
int tmpfs_rmdir(const char *path, int flags);
int tmpfs_rename(const char *oldpath, const char *newpath);
