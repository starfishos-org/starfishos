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
#include <chcore/idman.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <time.h>

#define MAX_FILENAME_LEN (255)
#define MAX_NR_FID_RECORDS 1024

#define FS_REG (1)
#define FS_DIR (2)
#define FS_SYM (3)

/* special errno branch number */
#define E_NAMEX_NOENT 201

#define PREFIX "[tmpfs]"
#define info(fmt, ...) fprintf(stderr, PREFIX " " fmt, ##__VA_ARGS__)
#if 0
#define debug(fmt, ...) \
	printf(PREFIX "<%s:%d>: " fmt, __func__, __LINE__, ##__VA_ARGS__)
#else
#define debug(fmt, ...) do { } while (0)
#endif
#define warn(fmt, ...) fprintf(stderr, PREFIX " " fmt, ##__VA_ARGS__)
#define error(fmt, ...) fprintf(stderr, PREFIX " " fmt, ##__VA_ARGS__)

struct string {
	char *str;
	size_t len;
	u64 hash;
};

struct dentry {
	struct string name;
	struct inode *inode;
	struct hlist_node node;
	int refcnt;
};

struct inode {
	int refcnt; /* Volatile reference. */
	int nlinks; /* Links */
	/* Stable inode number in the CXL disk image; never a virtual address. */
	u64 disk_ino;
	u64 type;
	size_t size; // FIXME(TCZ): this field should be coupled with data union
		     // member, e.g., should be accessed by means like
		     // `inode->reg.size`
	mode_t mode;
	union {
		struct htable dentries;
		struct radix data;
	};

	/* Address array, used by mmap. */
	struct {
		bool valid;
		vaddr_t *array;
		size_t nr_used; /* Number of entries filled. */
		size_t size;    /* Total capacity. */
		int translated_pmo_cap;    /* PMO cap of translated array. */
	} aarray;
};

struct super_block {
	u64 s_magic; /* 0x79F5 */
	u64 s_size;
	u64 s_root;
};

struct component {
	enum {
		/* All components store dentries. */
		COMPONENT_NORMAL = 0,
		/**
		 * If we resolving symlinks, no matter how deep,
		 * the components should be tagged with
		 * COMPONENT_SYMLINK.
		 */
		COMPONENT_SYMLINK = 1,
	} c_type;
	struct dentry *c_dentry;
};

struct path {
	/* A list of components. */
	int nr_comps;
	int max_comps;
	struct component *comps;
	int nr_symlink_resolving;
};

/* Opened fd entry */
struct fid_record {
	struct path path;
	struct inode *inode;
	u64 flags;
	off_t offset;
};

extern struct dentry *tmpfs_root_dent;
extern struct inode *tmpfs_root;
extern struct id_manager fidman;
extern struct fid_record fid_records[MAX_NR_FID_RECORDS];
