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