#include "util.h"
#include <stdlib.h>
#include <fs_vnode.h>
#include "defs.h"

int path_init(struct path *path)
{
	path->nr_comps = 0;
	path->max_comps = 8;
	path->nr_symlink_resolving = 0;
	path->comps = malloc(sizeof(*path->comps) * path->max_comps);
	return 0;
}

static inline int __path_append(struct path *path, struct dentry *dentry,
				int type)
{
	if (path->nr_comps == path->max_comps) {
		path->max_comps <<= 1;
		path->comps = realloc(path->comps,
				      sizeof(*path->comps) * path->max_comps);
	}
	path->comps[path->nr_comps].c_type = type;
	path->comps[path->nr_comps].c_dentry = get_dent(dentry);
	path->nr_comps++;
	return 0;
}

int path_append(struct path *path, struct dentry *dentry)
{
	return __path_append(path, dentry,
			     path->nr_symlink_resolving ? COMPONENT_SYMLINK :
			     COMPONENT_NORMAL);
}

int path_goback(struct path *path)
{
	if (path->nr_comps == 0)
		return 0;
	assert(path->nr_comps > 0);
	--path->nr_comps;
	put_dent(path->comps[path->nr_comps].c_dentry);
	return 0;
}

struct dentry *path_last_dent(struct path *path)
{
	assert(path->nr_comps > 0);
	return path->comps[path->nr_comps - 1].c_dentry;
}
struct inode *path_last_inode(struct path *path)
{
	assert(path->nr_comps > 0);
	return path_last_dent(path)->inode;
}

struct inode *path_last_dir_inode(struct path *path)
{
	int i;
	for (i = path->nr_comps - 1; i >= 0; --i) {
		struct inode *inode = path->comps[i].c_dentry->inode;
		debug("%d inode->type=%ld\n", i, inode->type);
		if (inode->type == FS_DIR)
			return inode;
		assert(inode->type == FS_SYM);
	}
	assert(0);
	return NULL;
}

int path_to_str(struct path *path, char *buf, int length, int expand_symlink)
{
	int i;

	buf[0] = '\0';
	for (i = 0; i < path->nr_comps; ++i) {
		if (path->comps[i].c_type == COMPONENT_SYMLINK &&
		    !expand_symlink)
			continue;
		/* [0] should be /. */
		if (i > 1)
			strncat(buf, "/", length);
		strncat(buf, path->comps[i].c_dentry->name.str,
			length);
		debug("%d: %s\n", i, path->comps[i].c_dentry->name.str);
	}
	return 0;
}

int path_copy(struct path *dst, struct path *src)
{
	int i;

	/* The dst should be empty. */
	assert(dst->nr_comps == 0);
	/* Enlarge if needed. */
	if (dst->max_comps < src->max_comps) {
		dst->comps = realloc(dst->comps,
				     sizeof(*dst->comps) * src->max_comps);
		dst->max_comps = src->max_comps;
	}
	/* Copy. */
	for (i = 0; i < src->nr_comps; ++i) {
		dst->comps[i].c_type = src->comps[i].c_type;
		dst->comps[i].c_dentry = get_dent(src->comps[i].c_dentry);
	}
	dst->nr_comps = src->nr_comps;
	dst->nr_symlink_resolving = 0;
	assert(src->nr_symlink_resolving == 0);
	return 0;
}

int path_fini(struct path *path)
{
	while (path->nr_comps)
		path_goback(path);
	path->max_comps = 0;
	free(path->comps);
	path->comps = NULL;
	return 0;
}

void path_enter_symlink(struct path *path)
{
	path->nr_symlink_resolving++;
}

void path_exit_symlink(struct path *path)
{
	path->nr_symlink_resolving--;
	assert(path->nr_symlink_resolving >= 0);
}

int path_in_symlink(struct path *path)
{
	return path->nr_symlink_resolving;
}

struct inode *new_inode(void)
{
	struct inode *inode = malloc(sizeof(*inode));

	if (!inode)
		return CHCORE_ERR_PTR(-ENOMEM);

	inode->refcnt = 0;
	inode->nlinks = 0;
	inode->type = 0;
	inode->size = 0;
	inode->aarray.valid = false;

	return inode;
}

int del_inode(struct inode *inode)
{
	if (inode->type == FS_REG || inode->type == FS_SYM) {
		// free radix tree
		radix_free(&inode->data);
		// free inode
		free(inode);
	} else if (inode->type == FS_DIR) {
		if (!htable_empty(&inode->dentries))
			return -ENOTEMPTY;

		// free htable
		htable_free(&inode->dentries);
		// free inode
		free(inode);
	} else {
		BUG("inode type that shall not exist");
	}
	return 0;
}

static int add_new_dent(struct inode *dir, const char *name, size_t len,
			struct inode *inode, struct dentry **dent)
{
	struct dentry *tmp_dent;

	tmp_dent = new_dent(inode, name, len);
	if (CHCORE_IS_ERR(tmp_dent))
		return CHCORE_PTR_ERR(tmp_dent);

	/* Get ref for the htable node. */
	get_dent(tmp_dent);
	htable_add(&dir->dentries, (u32)(tmp_dent->name.hash), &tmp_dent->node);
	dir->size++;
	inode->nlinks++;

	if (dent)
		*dent = get_dent(tmp_dent);

	return 0;
}


struct inode *new_dir(struct inode *parent)
{
	struct inode *inode;
	int err;

	inode = new_inode();
	if (CHCORE_IS_ERR(inode))
		return inode;

	get_inode(inode);

	inode->type = FS_DIR;
	init_htable(&inode->dentries, 1024);

	/* Add "." and "..". */
	err = add_new_dent(inode, ".", 1, inode, NULL);
	assert(!err);
	err = add_new_dent(inode, "..", 2, parent ? : inode, NULL);
	assert(!err);

	return inode;
}

struct inode *new_reg(void)
{
	struct inode *inode;

	inode = new_inode();
	if (CHCORE_IS_ERR(inode))
		return inode;

	get_inode(inode);

	inode->type = FS_REG;
	init_radix_w_deleter(&inode->data, free);

	return inode;
}

struct inode *new_symlink(const char *symlink)
{
	struct inode *inode;
	ssize_t link_written;
	size_t link_len = strlen(symlink);

	inode = new_inode();
	if (CHCORE_IS_ERR(inode))
		return inode;

	get_inode(inode);

	inode->type = FS_SYM;
	init_radix_w_deleter(&inode->data, free);

	link_written = tfs_file_write(inode, 0, symlink, link_len);
	assert(link_written == link_len);

	return inode;
}

/**
 * Note: assumes that each call to new_dent will result in an increment in inode
 * reference in the fs tree
 */
struct dentry *new_dent(struct inode *inode, const char *name, size_t len)
{
	struct dentry *dent;
	int err;

	/* TODO: use real malloc */
	dent = malloc(sizeof(*dent));
	if (!dent)
		return CHCORE_ERR_PTR(-ENOMEM);
	err = init_string(&dent->name, name, len);
	if (err) {
		free(dent);
		return CHCORE_ERR_PTR(err);
	}
	dent->inode = get_inode(inode);
	dent->refcnt = 0;

	return dent;
}

int del_dent(struct dentry *dent)
{
	free(dent);
	return 0;
}

int tfs_mknod(struct inode *dir, const char *name, size_t len, u64 file_type,
	      const char *symlink, struct dentry **dent)
{
	struct inode *inode;
	int err;

	BUG_ON(!name);

	if (len == 0) {
		WARN("tfs_remove with len of 0");
		return -ENOENT;
	}

	if (file_type == FS_DIR) {
		inode = new_dir(dir);
	} else if (file_type == FS_REG) {
		inode = new_reg();
	} else if (file_type == FS_SYM) {
		inode = new_symlink(symlink);
	} else {
		error("bad file_type\n");
		assert(0);
		return -1;
	}

	if (CHCORE_IS_ERR(inode))
		return CHCORE_PTR_ERR(inode);

	err = add_new_dent(dir, name, len, inode, dent);
	/* It could be complex to reclaim dir with "." and "..". */
	assert(!err);
	if (CHCORE_IS_ERR(err)) {
		free(inode);
		return err;
	}

	/* We have get_inode() in new_xxx(). */
	put_inode(inode);

	return 0;
}

int tfs_creat(struct inode *dir, const char *name, size_t len, mode_t mode,
	      struct dentry **dent)
{
	// TODO:
	// error("mode is not handled in creating files\n");
	assert(len > 0);
	return tfs_mknod(dir, name, len, FS_REG, /* symlink */ NULL, dent);
}

int tfs_mkdir(struct inode *dir, const char *name, size_t len, mode_t mode,
	      struct dentry **dent)
{
	// TODO:
	// error("mode is not handled in creating files\n");
	debug("name=%s\n", name);
	assert(len > 0);
	return tfs_mknod(dir, name, len, FS_DIR, /* symlink */ NULL, dent);
}

int tfs_truncate(struct inode *inode, size_t size)
{
	assert(inode->type == FS_REG);

	u64 page_no, page_off;
	void *page;
	if (size == 0) {
		// free radix tree and init an empty one
		radix_free(&inode->data);
		init_radix_w_deleter(&inode->data, free);
		inode->size = 0;
	}
	else if (size > inode->size) {
		/* truncate should not allocate the space for the file */
		inode->size = size;
	}
	else if (size < inode->size) {
		size_t cur_off = size;
		size_t to_write;
		page_no = cur_off / PAGE_SIZE;
		page_off = cur_off % PAGE_SIZE;
		if (page_off) {
			/* if the last write cannot fill the last page, set the
			 * remaining space to zero to ensure the correctness of
			 * the file_read */
			page = radix_get(&inode->data, page_no);
			if (page) {
				to_write = MIN(inode->size - size,
					       PAGE_SIZE - page_off);
				memset(page + page_off, 0, to_write);
				cur_off += to_write;
			}
		}
		while (cur_off < inode->size) {
			page_no = cur_off / PAGE_SIZE;

			radix_del(&inode->data, page_no, 1);

			cur_off += PAGE_SIZE;
		}
		inode->size = size;
	}

	return 0;
}

int tfs_allocate(struct inode *inode, off_t offset, off_t len, int keep_size)
{
	assert(inode->type == FS_DIR || inode->type == FS_REG);

	u64 page_no;
	u64 cur_off = offset;
	void *page;

	while (cur_off < offset + len) {
		page_no = cur_off / PAGE_SIZE;

		page = radix_get(&inode->data, page_no);
		if (!page) {
			page = malloc(PAGE_SIZE);
			if (!page)
				return -ENOSPC;
			if (radix_add(&inode->data, page_no, page))
			{
				free(page);
				return -ENOSPC;
			}
			memset(page, 0, PAGE_SIZE);
		}
		cur_off += PAGE_SIZE;
	}

	if (offset + len > inode->size && !keep_size) {
		inode->size = offset + len;
	}
	return 0;
}

int tfs_remove(struct inode *dir, const char *name, size_t len)
{
	u64 hash = hash_chars(name, (ssize_t)len);
	struct dentry *dent, *target = NULL;
	struct hlist_head *head;
	int b;
	int err;

	BUG_ON(!name);

	if (len == 0) {
		WARN("tfs_remove with len of 0");
		return -ENOENT;
	}

	head = htable_get_bucket(&dir->dentries, (u32)hash);

	for_each_in_hlist(dent, node, head)
	{
		if (dent->name.len == len &&
		    0 == strcmp(dent->name.str, name)) {
			target = dent;
			break;
		}
	}

	if (!target)
		return -ENOENT;

	BUG_ON(!target->inode);

	/* rmdir only remove empty dir */
	if (target->inode->type == FS_DIR) {
		for_each_in_htable(dent, b, node, &target->inode->dentries) {
			if (strcmp(".", dent->name.str) != 0 && strcmp("..", dent->name.str) != 0)
				return -ENOTEMPTY;
		}

	}

	// FIXME(TCZ): remove only when file is closed by all processes
	target->inode->nlinks--;
	if ((err = put_inode(target->inode))) {
		target->inode->nlinks++;
		return err;
	}
	// remove dentry from parent
	htable_del(&target->node);
	// free dentry
	free(target);

	return 0;
}

// utility for writing regular files and symbolic links
ssize_t tfs_file_write(struct inode *inode, off_t offset, const char *data,
		       size_t size)
{
	BUG_ON(inode->type == FS_DIR);

	u64 page_no, page_off;
	u64 cur_off = offset;
	size_t to_write;
	void *page = NULL;

	if (size == 0)
		return 0;

	while (size > 0) {
		page_no = cur_off / PAGE_SIZE;
		page_off = cur_off % PAGE_SIZE;

		page = radix_get(&inode->data, page_no);
		if (!page) {
			/**
			 * NOTE(MK): All pages should be page-aligned. This is
			 * necessary for serving fmap.
			 */
			page = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
			if (!page)
				return (ssize_t)(cur_off - offset);
			/**
			 * NOTE(HYQ): If front bytes in page is in hole, filling
			 * with zeros
			 */
			if (page_off)
				memset(page, 0, page_off);
			radix_add(&inode->data, page_no, page);
		}

		BUG_ON(page == NULL);

		to_write = MIN(size, PAGE_SIZE - page_off);
		memcpy(page + page_off, data, to_write);
		cur_off += to_write;
		data += to_write;
		size -= to_write;
	}

	if (cur_off > inode->size) {
		inode->size = cur_off;
		if (cur_off % PAGE_SIZE && page) {
			/* if the last write cannot fill the last page, set the
			 * remaining space to zero to ensure the correctness of
			 * the file_read */
			page_off = cur_off % PAGE_SIZE;
			memset(page + page_off, 0, PAGE_SIZE - page_off);
		}
	}
	return (ssize_t)(cur_off - offset);
}

ssize_t tfs_file_read(struct inode *inode, off_t offset, char *buff,
		      size_t size)
{
	BUG_ON(inode->type == FS_DIR);

	u64 page_no, page_off;
	u64 cur_off = offset;
	size_t to_read;
	void *page;

	/* Returns 0 according to man pages. */
	if (offset >= inode->size)
		return 0;


	// kinfo("offset=%d, size=%d, inode->size=%d\n", offset, size,
	// inode->size);
	size = MIN(inode->size - offset, size);

	while (size > 0 && cur_off <= inode->size) {
		page_no = cur_off / PAGE_SIZE;
		page_off = cur_off % PAGE_SIZE;

		page = radix_get(&inode->data, page_no);
		to_read = MIN(size, PAGE_SIZE - page_off);
		if (!page)
			memset(buff, 0, to_read);
		else
			memcpy(buff, page + page_off, to_read);
		cur_off += to_read;
		buff += to_read;
		size -= to_read;
	}

	return (ssize_t)(cur_off - offset);
}

int tfs_namex_lookup(struct inode *dirat, const char *name, int mkdir_p,
		     struct dentry **dent)
{
	int err;

	/* goto next component */
	if (CHCORE_IS_ERR(*dent = tfs_lookup(dirat, name, strlen(name)))) {

		return CHCORE_PTR_ERR(*dent);
	}

	if (*dent == NULL) {
		/* called by mkdir_p, create the intermediate
		 * directories as required */
		if (mkdir_p) {
			error("mode is ignored in namex with mkdir_p=1");
			if ((err = tfs_mkdir(dirat, name, strlen(name),
					     /* mode */ 0, dent)))
				return err;

			BUG_ON(dent == NULL);
		} else
			return -E_NAMEX_NOENT;
	}

	return 0;
}

/**
 * Lookup the `*name`, saves parent dir to `*dirat` and child's name to `*name`.
 * If `*name` starts with '/', then lookup starts from `tmpfs_root`, else from
 * `*dirat`.
 *
 * If `follow_symlink` is 1, then tfs_namex will follow through the symbolic
 * link. However, no follow-through will happen if the final component is a
 * symbolic link.
 *
 * Note that when `*name` ends with '/', the inode of last component will be
 * saved in `*dirat` regardless of its type (e.g., even when it's FS_REG) and
 * `*name` will point to '\0'
 */
int tfs_namex(const char **name, struct path *path,
	      int mkdir_p, int follow_symlink)
{
	struct inode *parent_inode;
	BUG_ON(name == NULL);
	BUG_ON(*name == NULL);

	char buff[MAX_FILENAME_LEN + 1];
	int i;
	struct dentry *dent = NULL;
	int err;

	if (**name == '/') {
		assert(path_in_symlink(path));
		info("do not support symlink to a absolute path.\n");
		assert(0);
	}

	parent_inode = path_last_dir_inode(path);

	// make sure a child name exists
	if (!**name)
		return -EINVAL;

	// each loop begins with an actual name
	do {
		/* parse next component */
		for (i = 0; **name && **name != '/'; ++i) {
			if (i >= MAX_FILENAME_LEN) {
				printf("pathname contains component larger than"
				       " MAX_FILENAME_LEN: %s",
				       *name);
				return -EINVAL;
			}
			buff[i] = *(*name)++;
		}
		buff[i] = '\0';
		if (!**name) {
			/* reach the tail of the name */
			/* i is the length of the name in buff */
			*name -= i;
			return 0;
		}

		if ((err = tfs_namex_lookup(parent_inode,
					    buff, mkdir_p, &dent)))
			return err;

		path_append(path, dent);
		parent_inode = dent->inode;
		/**
		 * For symlink: the symlink file dentry is not tagged with
		 * COMPONENT_SYMLINK, the target dentry is tagged.
		 * Supppose /a/b is a symlink to c/e/f.
		 * In parsing /a/b/g, the dentries for c, e, and f is tagged
		 * with COMPONENT_SYMLINK.
		 */

		/* We always following symlinks in the middle of the path. */
		if (dent->inode->type == FS_SYM) {
			char link[dent->inode->size];
			const char *link_p = link;
			tfs_file_read(dent->inode, 0, link, dent->inode->size);

			path_enter_symlink(path);

			err = tfs_namex(&link_p, path, mkdir_p, true);
			if (err) {
				/* TODO(MK): Reclaim resources. */
				return err;
			}

			assert (*link_p != '\0');
			err = tfs_namex_lookup(path_last_inode(path),
					       link_p, mkdir_p, &dent);
			if (err) {
				/* TODO(MK): Reclaim resources. */
				return err;
			}

			assert(dent->inode->type == FS_DIR);

			path_append(path, dent);

			parent_inode = path_last_inode(path);

			path_exit_symlink(path);
		}

		/* skip '/'s */
		while (**name && **name == '/')
			++(*name);

		if (**name && path_last_inode(path)->type != FS_DIR)
			return -ENOTDIR;
	} while (true);
}

// TODO: handle . and ..
struct dentry *tfs_lookup(struct inode *dir, const char *name, size_t len)
{
	u64 hash = hash_chars(name, (ssize_t)len);
	struct dentry *dent;
	struct hlist_head *head;

	head = htable_get_bucket(&dir->dentries, (u32)hash);

	for_each_in_hlist(dent, node, head)
	{
		if (dent->name.len == len && 0 == strcmp(dent->name.str, name))
			return dent;
	}
	return NULL;
}

/**
 * under walk->parent, starts from walk->path, will be recursively called when
 * encountering symbolic link.
 */
int handle_xxxat_internal(struct tmpfs_walk_path *walk)
{
	struct inode *parent;
	int ret;
	ssize_t bytes_read;

	assert(walk->path_len > 0);
	while (walk->cursor < walk->path_len && walk->path[walk->cursor] == '/')
		/* Eat all '/'s. */
		walk->cursor++;
	debug("walk->path=%s\n", walk->path);
	walk->leaf = &walk->path[walk->cursor];

	if (walk->cursor == walk->path_len) {
		/* The target is current inode. */
		walk->target = path_last_inode(&walk->path_record);
		goto found;
	}
	debug("walk->leaf=%s\n", walk->leaf);

	/* Go through the path before the last component. */
	/* TODO(MK): handle mount-points in tfs_namex. */
	if ((ret = tfs_namex(&walk->leaf, &walk->path_record,
			     /* mkdir_p */ 0,
			     /* resolving symlink */ 1)))
		goto error;

	parent = path_last_inode(&walk->path_record);

	/* when path ends with '/', like /a/b/ */
	if (*walk->leaf == '\0') {
		if (parent->type != FS_DIR) {
			ret = -ENOENT;
			goto error;
		}
		/* This is .. somewhat wierd. */
		walk->target = parent;
		goto found;
	}

	/* Lookup the last component (the target file). */
	walk->target_dent = tfs_lookup(parent, walk->leaf, strlen(walk->leaf));

	/* if is symlink */
	if (walk->target_dent && walk->target_dent->inode->type == FS_SYM &&
	    walk->follow_symlink) {
		printf("fuck\n");

		/**
		 * The symlink itself is not tagged with COMPONENT_SYMLINK.
		 * See the notes in namex.
		 */
		path_append(&walk->path_record, walk->target_dent);

		path_enter_symlink(&walk->path_record);

		BUG_ON(walk->target_dent->inode->size <= 0);

		assert(FS_REQ_PATH_LEN >= walk->target_dent->inode->size);
		bytes_read = tfs_file_read(walk->target_dent->inode, 0,
				    walk->symlink_buf,
				    walk->target_dent->inode->size);
		assert(bytes_read == walk->target_dent->inode->size);
		walk->symlink_buf[bytes_read] = '\0';

		walk->path = walk->symlink_buf;
		walk->path_len = walk->target_dent->inode->size;
		walk->cursor = 0;
		if (walk->symlink_buf[0] == '/') {
			path_append(&walk->path_record, tmpfs_root_dent);
		}

		ret = handle_xxxat_internal(walk);

		path_exit_symlink(&walk->path_record);

		return ret;
	}

	/* No such file. */
	if (!walk->target_dent && walk->not_found_callback) {
		if ((ret = walk->not_found_callback(ret, walk,
						    walk->callback_data)))
			goto error;
	}

	if (walk->target_dent) {
		walk->target = walk->target_dent->inode;
	} else {
		ret = -ENOENT;
		debug("no such file %s\n", walk->leaf);
		goto error;
	}
found:
	return 0;
error:
	debug("%s: ret=%d\n", __func__, ret);
	return ret;
}

int handle_xxxat(int dirfd, struct tmpfs_walk_path *walk)
{
	int err;
	struct server_entry *entry;

	debug("%s: dirfd=%d walk=%p\n", __func__, dirfd, walk);

	walk->cursor = 0;
	walk->target = NULL;
	walk->target_dent = NULL;
	walk->leaf = NULL;
	/* TODO(MK): Support recursive mounting. */
	walk->next_fs_idx = -1;		      /* No next fs. */
	walk->path_advanced = walk->path_len; /* The whole path is handled. */
	path_init(&walk->path_record);

	assert(dirfd != AT_FDCWD);
	assert(walk->path_len > 0);

	if (walk->path[0] == '/') {
		assert(dirfd == AT_FDROOT);
	}

	if (dirfd == AT_FDROOT) {
		debug("AT_FDROOT\n");
		path_append(&walk->path_record, tmpfs_root_dent);
	} else {
		debug("AT_ %d\n", dirfd);
		entry = server_entrys[dirfd];
		if (!entry) {
			err = -EBADF;
			goto error;
		}
		// TODO(TCZ): check if fid_record open for reading or searching
		path_copy(&walk->path_record, (struct path *)entry->path);
	}
	assert(path_last_inode(&walk->path_record));

	return handle_xxxat_internal(walk);

error:
	debug("%s: err=%d\n", __func__, err);
	return err;
}
