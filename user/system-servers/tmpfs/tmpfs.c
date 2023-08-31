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

#include <chcore-internal/fs_debug.h>
#include <fs_wrapper_defs.h>
#include <fs_vnode.h>
#include "tmpfs.h"

struct inode *tmpfs_root = NULL;
struct dentry *tmpfs_root_dent = NULL;
struct id_manager fidman;
struct fid_record fid_records[MAX_NR_FID_RECORDS];

bool mounted;
bool using_page_cache;

#ifdef TEST_COUNT_PAGE_CACHE
struct test_count count;
#endif

struct server_entry *server_entrys[MAX_SERVER_ENTRY_NUM];
RBTree *fs_vnode_list;

int tfs_load_image(const char *start)
{
	struct cpio_file *f;
	struct inode *dirat;
	struct dentry *dent;
	const char *leaf;
	size_t len;
	int err;
	ssize_t write_count;
	struct path path;

	BUG_ON(start == NULL);

	cpio_init_g_files();
	cpio_extract(start, "/");

	// TODO:
	// error("mode is not handled in tfs_load_image\n");

	for (f = g_files.head.next; f; f = f->next) {
		if (!(f->header.c_filesize))
			continue;

		leaf = f->name;

		path_init(&path);
		path_append(&path, tmpfs_root_dent);

		err = tfs_namex(&leaf, &path,
				/* mkdir_p */ 1,
				/* follow_symlink */ 1);
		if (err) {
			goto error;
		}

		dirat = path_last_inode(&path);
		assert(dirat->type == FS_DIR);
		/* cpio will include the path in the end, so we should skip the
		 * directories that have been created */
		len = strlen(leaf);
		dent = tfs_lookup(dirat, leaf, len);
		if (dent) {
			path_fini(&path);
			continue;
		}

		err = tfs_creat(dirat, leaf, len, /* mode */ 0, NULL);
		if (err)
			goto error;

		dent = tfs_lookup(dirat, leaf, len);
		BUG_ON(dent == NULL);

		len = f->header.c_filesize;
		write_count = tfs_file_write(dent->inode, 0, f->data, len);
		if (write_count != len) {
			err = -ENOSPC;
			goto error;
		}
	}

	return 0;

error:
	path_fini(&path);
	return err;
}

int init_tmpfs(void)
{
	tmpfs_root = new_dir(NULL);
	tmpfs_root_dent = new_dent(tmpfs_root, "/", 1);

	init_id_manager(&fidman, MAX_NR_FID_RECORDS, DEFAULT_INIT_ID);
	/**
	 * Allocate the first id, which should be 0.
	 * No request should use 0 as the fid.
	 */
	assert(alloc_id(&fidman) == 0);

	init_fs_wrapper();

	/* tmpfs should never use page cache. */
	using_page_cache = false;
	BUG_ON(using_page_cache);

	mounted = true;

	return 0;
}

extern char __binary_ramdisk_cpio_start;

int main(int argc, char *argv[], char *envp[])
{
	init_tmpfs();

	tfs_load_image((char *)&__binary_ramdisk_cpio_start);
	info("[tmpfs] register server value = %u\n",
	     ipc_register_server(fs_server_dispatch,
				 DEFAULT_CLIENT_REGISTER_HANDLER));

	usys_exit(0);
	return 0;
}
