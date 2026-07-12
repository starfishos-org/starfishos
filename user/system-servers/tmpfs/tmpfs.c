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
#include "plog.h"
#include "tmpfs_polling.h"

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

int init_tmpfs(char *mount_path)
{
	tmpfs_root = new_dir(NULL);
	tmpfs_root_dent = new_dent(tmpfs_root, mount_path, strlen(mount_path));
	/* Permanent reference: prevents tmpfs_root_dent from being freed when
	 * path_fini drops the transient get_dent added by path_append. */
	get_dent(tmpfs_root_dent);

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

int restart_tmpfs(char *mount_path)
{
	init_tmpfs(mount_path);
	tfs_load_image((char *)&__binary_ramdisk_cpio_start);
	info("[tmpfs] register server value = %u\n",
	ipc_register_server(fs_server_dispatch,
			DEFAULT_CLIENT_REGISTER_HANDLER));
	usys_exit(0);
	return 0;
}

/*
 * Recovery mode: replay p-log from a crashed machine and take over.
 *
 * 1. Init empty tmpfs
 * 2. Map and replay the crashed machine's p-log
 * 3. Init own p-log
 * 4. Register as IPC server
 * 5. Send FSM_REQ_REPLACE_FS to FSM to take over the "/" mount point
 */
static long timespec_diff_ms(struct timespec *start, struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000L +
	       (end->tv_nsec - start->tv_nsec) / 1000000L;
}

int recover_tmpfs(int crashed_machine_id)
{
	struct timespec t0, t1, t2;

	info("[tmpfs] Recovery mode: replaying p-log from machine %d\n",
	     crashed_machine_id);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	/* 1. Init empty tmpfs */
	init_tmpfs("/");

	/* 2. Map and replay crashed machine's p-log */
	struct plog_header *remote_plog = plog_map_remote(crashed_machine_id);
	if (!remote_plog) {
		error("[tmpfs] Failed to map remote p-log\n");
		return -1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t1);
	int ret = plog_replay(remote_plog);
	clock_gettime(CLOCK_MONOTONIC, &t2);

	info("[TIMING] plog_recovery: %ld ms\n", timespec_diff_ms(&t1, &t2));
	if (ret < 0)
		info("[tmpfs] P-log replay had errors (continuing anyway)\n");

	/* 3. Init own p-log */
	int mid = usys_get_machine_id();
	plog_init(mid);

	/* 4. Register as IPC server */
	info("[tmpfs] Registering as IPC server...\n");
	int reg_ret = ipc_register_server(fs_server_dispatch,
	                                  DEFAULT_CLIENT_REGISTER_HANDLER);
	info("[tmpfs] register server value = %u\n", reg_ret);

	struct timespec t3;
	clock_gettime(CLOCK_MONOTONIC, &t3);
	info("[TIMING] fs_recovery_total: %ld ms\n", timespec_diff_ms(&t0, &t3));

	/* 5. Register with FSM to replace the mount point */
	/* TODO: Send FSM_REQ_REPLACE_FS to FSM.
	 * For now, the polling path bypasses FSM, so this step is
	 * deferred to when direct IPC recovery is needed. */

	info("plog_recovery: done\n");
	return 0;
}

int main(int argc, char *argv[], char *envp[])
{
	extern int global_memory_malloc_type;
	global_memory_malloc_type = MALLOC_TYPE_SHARED;

	/* Check for recovery mode: tmpfs --recover <machine_id> */
	if (argc >= 3 && strcmp(argv[1], "--recover") == 0) {
		int crashed_mid = atoi(argv[2]);
		recover_tmpfs(crashed_mid);
		usys_exit(0);
		return 0;
	}

	struct timespec fs_t0, fs_t1;
	clock_gettime(CLOCK_MONOTONIC, &fs_t0);

	init_tmpfs("/");

	/* Initialize p-log on CXL for this machine's tmpfs */
	int mid = usys_get_machine_id();
	plog_init(mid);

	tfs_load_image((char *)&__binary_ramdisk_cpio_start);

	/* tmpfs polling thread disabled — polling.srv is the sole SHM consumer.
	 * Code preserved in tmpfs_polling.c for potential future use. */
	/*
	printf("[tmpfs] Initializing polling thread...\n");
	if (tmpfs_start_polling_thread() != 0) {
		printf("[tmpfs] Failed to start polling thread\n");
	} else {
		printf("[tmpfs] Polling thread started successfully\n");
	}
	*/

	info("[tmpfs] register server value = %u\n",
	     ipc_register_server(fs_server_dispatch,
				 DEFAULT_CLIENT_REGISTER_HANDLER));

	clock_gettime(CLOCK_MONOTONIC, &fs_t1);
	info("[TIMING] fs_startup: %ld ms\n", timespec_diff_ms(&fs_t0, &fs_t1));

	usys_exit(0);
	return 0;
}
