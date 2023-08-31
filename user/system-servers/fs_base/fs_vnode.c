

#include "fs_vnode.h"
#include "fs_page_cache.h"
#include <chcore-internal/fs_debug.h>

void free_entry(int entry_idx)
{
	free(server_entrys[entry_idx]->path);
	free(server_entrys[entry_idx]);
	server_entrys[entry_idx] = NULL;
}

int alloc_entry()
{
	int i;

	for (i = 0; i < MAX_SERVER_ENTRY_NUM; i++) {
		if (server_entrys[i] == NULL) {
			server_entrys[i] =
				(struct server_entry *)malloc(sizeof(struct server_entry));
			if (server_entrys[i] == NULL)
				return -1;
			pthread_mutex_init(&server_entrys[i]->lock, NULL);
			fs_debug_trace_fswrapper("entry_id=%d\n", i);
			return i;
		}
	}
	return -1;
}

void assign_entry(struct server_entry *e, u64 f, off_t o, int t, void *p, struct fs_vnode *n)
{
	fs_debug_trace_fswrapper("flags=0x%lo, offset=0x%ld, path=%s, vnode_id=%ld\n",
							  f, o, (char *)p, n->vnode_id);
	e->flags = f;
	e->offset = o;
	e->path = p;
	e->vnode = n;
	e->refcnt = t;
}

void fs_vnode_init()
{
	fs_vnode_list = new_rbtree();
}

struct fs_vnode *alloc_fs_vnode(ino_t id, enum fs_vnode_type type,
					size_t size, void *private)
{
	struct fs_vnode *ret = (struct fs_vnode *)malloc(sizeof(*ret));

	/* Filling Initial State */
	ret->vnode_id = id;
	ret->type = type;
	ret->size = size;
	ret->private = private;
	fs_debug_trace_fswrapper("id=%ld, type=%d, size=%ld(0x%lx)\n", id, type, size, size);

	/* Ref Count start as 1 */
	ret->refcnt = 1;

	/**
	 * NOTE: PMO_FILE is not created immediately at allocation time.
	 * When a vnode first mmaped,
	 * 	trigger a create_pmo for this vnode lazily.
	 */
	ret->pmo_cap = -1;

	/* Create a page cache entity for vnode */
	extern bool using_page_cache;
	if (using_page_cache)
		ret->page_cache = new_page_cache_entity_of_inode(ret->vnode_id, ret);
	pthread_rwlock_init(&ret->rwlock, NULL);

	return ret;
}

void push_fs_vnode(struct fs_vnode *n)
{
	RBNode *rbnode = new_rbnode((rbkey_t)n->vnode_id, (rbvalue_t)n);
	rbtree_insert(fs_vnode_list, rbnode);
}

void pop_free_fs_vnode(struct fs_vnode *n)
{
	RBNode *rbnode = rbtree_delete(fs_vnode_list, (rbkey_t)n->vnode_id);
	free_rbnode(rbnode);
	free(n);
}

struct fs_vnode *get_fs_vnode_by_id(ino_t vnode_id)
{
	RBNode *rbnode = rbtree_get(fs_vnode_list, (rbkey_t)vnode_id);
	if (!rbnode)
		return NULL;
	return (struct fs_vnode *)rbnode->value;
}

/* refcnt for vnode */
int inc_ref_fs_vnode(void *n)
{
	((struct fs_vnode *)n)->refcnt++;
	return 0;
}

int dec_ref_fs_vnode(void *node)
{
	int ret;
	struct fs_vnode *n = (struct fs_vnode *)node;

	n->refcnt--;
	assert(n->refcnt >= 0);

	if (n->refcnt == 0) {
		/* TODO(HYQ): concurrency control of vnode */

		/*
		 * NOTE(HYQ): There is only ONE copy of private in vnode (for each file),
		 *			  Others are revoked during open.
		 */
		ret = server_ops.close(n->private, (n->type == FS_NODE_DIR));
		if (ret) {
			printf("Warning: close failed when deref vnode: %d\n", ret);
			return ret;
		}

		/*
		 * NOTE(HYQ): refcnt == 0 means no pages in pce,
		 *  		So we don't need to evict pages from page cache
		 */

		pop_free_fs_vnode(n);
	}

	return 0;
}