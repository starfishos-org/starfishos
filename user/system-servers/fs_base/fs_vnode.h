#pragma once

#include <chcore/type.h>
#include <assert.h>
#include <sys/types.h>
#include <chcore/container/list.h>
#include <chcore/container/hashtable.h>
#include <chcore/container/rbtree.h>
#include "fs_page_cache.h"
#include "fs_wrapper_defs.h"

#define MAX_FILE_PAGES 512
#define MAX_SERVER_ENTRY_NUM 1024

enum fs_vnode_type {
	FS_NODE_RESERVED = 0,
	FS_NODE_REG,
	FS_NODE_DIR
};

/*
 * per-inode
 */
#define PC_HASH_SIZE 512
struct fs_vnode {
	ino_t vnode_id;				/* identifier */

	enum fs_vnode_type type;		/* regular or directory */
	int refcnt;				/* reference count */
	size_t size;				/* file size or directory entry number */
	struct page_cache_entity_of_inode *page_cache;
	int pmo_cap;				/* fmap fault is handled by this */
	void *private;

	pthread_rwlock_t rwlock;		/* vnode rwlock */
};

/*
 * per-fd
 */
struct server_entry {
	/* `flags` and `offset` is assigned to each fd */
	int flags;
	off_t offset;
	int refcnt;
	/* Different FS may use different struct to store path, normally `char*` */
	void *path;

	/* Entry lock */
	pthread_mutex_t lock;

	/* Each vnode is binding with a disk inode */
	struct fs_vnode *vnode;
};

extern struct server_entry *server_entrys[MAX_SERVER_ENTRY_NUM];

extern void free_entry(int entry_idx);
extern int alloc_entry();
extern void assign_entry(struct server_entry *e, u64 f, off_t o, int t, void *p, struct fs_vnode *n);

/*
 * fs_vnode pool
 * key: ino_t vnode_id
 * value: struct fs_vnode *vnode
 */
extern RBTree *fs_vnode_list;

extern void fs_vnode_init();
extern struct fs_vnode *alloc_fs_vnode(ino_t id, enum fs_vnode_type type,
						size_t size, void *private);
extern void push_fs_vnode(struct fs_vnode *n);
extern void pop_free_fs_vnode(struct fs_vnode *n);
extern struct fs_vnode *get_fs_vnode_by_id(ino_t vnode_id);

int inc_ref_fs_vnode(void *);
int dec_ref_fs_vnode(void *);

