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
#include "cxlfs.h"
#include "tmpfs_polling.h"

struct inode *tmpfs_root = NULL;
struct dentry *tmpfs_root_dent = NULL;
struct id_manager fidman;
struct fid_record fid_records[MAX_NR_FID_RECORDS];
int cxlfs_boot_data_validated;
int cxlfs_mount_was_fresh;
u64 cxlfs_boot_data_checksum;

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

static const char *cpio_relative_name(const char *name)
{
	while (*name == '/')
		++name;
	while (name[0] == '.' && name[1] == '/')
		name += 2;
	return name;
}

/*
 * Verify a boot-critical file byte-for-byte against the embedded image.  This
 * checks the complete path: restored directory entry -> stable inode number ->
 * CXL block mapping -> file data.  A server that fails this check must never
 * become the machine's default root filesystem.
 */
static int verify_cxlfs_boot_data(const char *archive)
{
	const char *probe_name = "libc.so";
	struct cpio_file *expected = NULL;
	ino_t vnode_id;
	size_t vnode_size;
	int vnode_type;
	void *inode;
	char path[FS_REQ_PATH_BUF_LEN];
	char *buf;

	cpio_init_g_files();
	cpio_extract(archive, "/");
	for (struct cpio_file *f = g_files.head.next; f; f = f->next) {
		if (!strcmp(cpio_relative_name(f->name), probe_name)) {
			expected = f;
			break;
		}
	}
	if (!expected || expected->header.c_filesize == 0)
		return -ENOENT;

	snprintf(path, sizeof(path), "/%s", probe_name);
	int ret = tmpfs_open(path, O_RDONLY, 0, &vnode_id, &vnode_size,
			     &vnode_type, &inode);
	if (ret)
		return ret;
	if (vnode_type != FS_NODE_REG ||
	    vnode_size != expected->header.c_filesize) {
		error("[CXLFS] %s size mismatch: disk=%lu image=%lu\n",
		      path, (unsigned long)vnode_size,
		      (unsigned long)expected->header.c_filesize);
		tmpfs_close(inode, false);
		return -EIO;
	}

	buf = malloc(PAGE_SIZE);
	if (!buf) {
		tmpfs_close(inode, false);
		return -ENOMEM;
	}
	u64 checksum = 1469598103934665603ULL;
	const unsigned char *expected_data =
		(const unsigned char *)expected->data;
	for (size_t off = 0; off < vnode_size; off += PAGE_SIZE) {
		size_t len = vnode_size - off;
		if (len > PAGE_SIZE)
			len = PAGE_SIZE;
		ssize_t nr = tmpfs_read(inode, off, len, buf);
		if (nr != (ssize_t)len) {
			error("[CXLFS] %s short read at %lu: got=%ld expected=%lu\n",
			      path, (unsigned long)off, (long)nr,
			      (unsigned long)len);
			free(buf);
			tmpfs_close(inode, false);
			return -EIO;
		}
		for (size_t i = 0; i < len; ++i) {
			unsigned char actual = (unsigned char)buf[i];
			if (actual != expected_data[off + i]) {
				error("[CXLFS] %s data mismatch at offset %lu: "
				      "disk=%02x image=%02x\n",
				      path, (unsigned long)(off + i), actual,
				      expected_data[off + i]);
				free(buf);
				tmpfs_close(inode, false);
				return -EIO;
			}
			checksum ^= actual;
			checksum *= 1099511628211ULL;
		}
	}
	free(buf);
	tmpfs_close(inode, false);
	cxlfs_boot_data_checksum = checksum;
	return 0;
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

static int replace_root_mount(void)
{
	ipc_msg_t *msg = ipc_create_msg(fsm_ipc_struct,
					 sizeof(struct fsm_request), 0);
	if (!msg)
		return -ENOMEM;
	struct fsm_request *req =
		(struct fsm_request *)ipc_get_msg_data(msg);
	memset(req, 0, sizeof(*req));
	req->req = FSM_REQ_REPLACE_FS;
	strcpy(req->mount_path, "/");
	req->mount_path_len = 1;
	req->target_machine_id = usys_get_machine_id();
	int ret = ipc_call(fsm_ipc_struct, msg);
	ipc_destroy_msg(msg);
	return ret;
}

int recover_tmpfs(int crashed_machine_id)
{
	struct timespec t0, t1, t2;

	info("[tmpfs] Recovery mode: replaying p-log from machine %d\n",
	     crashed_machine_id);

	clock_gettime(CLOCK_MONOTONIC, &t0);

	/* 1. Map the crashed machine's CXL-resident p-log. */
	struct plog_header *remote_plog = plog_map_remote(crashed_machine_id);
	if (!remote_plog) {
		error("[tmpfs] Failed to map remote p-log\n");
		return -1;
	}

	/* 2. Mount the stable CXL disk image and rebuild volatile indices. */
	int fs_machine = plog_fs_machine(remote_plog);
	if (fs_machine < 0) {
		error("[tmpfs] P-log has no valid CXL FS identity\n");
		return -1;
	}
	init_tmpfs("/");
	int fresh = cxlfs_mount(fs_machine, tmpfs_root);
	if (fresh != 0) {
		error("[tmpfs] Missing persistent CXL FS image for machine %d\n",
		      fs_machine);
		return -1;
	}
	if (cxlfs_restore_tree(tmpfs_root) < 0) {
		error("[tmpfs] Failed to rebuild tmpfs indices from CXL FS\n");
		return -1;
	}
	info("[tmpfs] Mounted CXL FS generation %lu (owner=%d)\n",
	     (unsigned long)cxlfs_generation(), fs_machine);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	int ret = plog_replay(remote_plog);
	clock_gettime(CLOCK_MONOTONIC, &t2);
	{
		char current_path[] = "/tmp/leveldb_recovery/CURRENT";
		ino_t vnode_id;
		size_t vnode_size;
		int vnode_type;
		void *inode;
		int current_ret = tmpfs_open(current_path, O_RDONLY, 0,
					     &vnode_id, &vnode_size,
					     &vnode_type, &inode);
		info("[CXLFS] recovered CURRENT lookup: ret=%d size=%lu\n",
		     current_ret, current_ret ? 0UL : (unsigned long)vnode_size);
		if (!current_ret)
			tmpfs_close(inode, false);
	}

	printf("[TIMING] plog_recovery: %ld ms\n", timespec_diff_ms(&t1, &t2));
	if (ret < 0)
		info("[tmpfs] P-log replay had errors (continuing anyway)\n");

	/* 3. Init own p-log */
	int mid = usys_get_machine_id();
	plog_init(mid);
	cxlfs_mount_was_fresh = 0;
	int verify_ret = verify_cxlfs_boot_data(
		(char *)&__binary_ramdisk_cpio_start);
	if (verify_ret < 0) {
		error("[CXLFS] Recovered data verification failed: %d\n",
		      verify_ret);
		return verify_ret;
	}
	cxlfs_boot_data_validated = 1;
	if (plog_checkpoint() == 0)
		plog_truncate();

	/* 4. Register as IPC server */
	info("[tmpfs] Registering as IPC server...\n");
	int reg_ret = ipc_register_server(fs_server_dispatch,
	                                  DEFAULT_CLIENT_REGISTER_HANDLER);
	info("[tmpfs] register server value = %u\n", reg_ret);

	/* 5. Atomically switch pathname and fmap-fault routing to this server. */
	ret = replace_root_mount();
	if (ret < 0) {
		error("[tmpfs] Failed to replace the root mount: %d\n", ret);
		return ret;
	}
	struct timespec t3;
	clock_gettime(CLOCK_MONOTONIC, &t3);
	printf("[TIMING] fs_recovery_total: %ld ms\n", timespec_diff_ms(&t0, &t3));

	info("plog_recovery: done\n");
	return 0;
}

int main(int argc, char *argv[], char *envp[])
{
	extern int global_memory_malloc_type;
	/* inode/dentry/hash/fd state is a disposable DRAM projection. */
	global_memory_malloc_type = MALLOC_TYPE_DEFAULT;

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
	int mid = usys_get_machine_id();
	int fresh = cxlfs_mount(mid, tmpfs_root);
	if (fresh < 0) {
		error("[tmpfs] Failed to mount CXL FS: %d\n", fresh);
		return fresh;
	}
	struct plog_header *local_plog = plog_init(mid);
	if (!local_plog)
		return -1;
	cxlfs_mount_was_fresh = fresh;
	if (fresh) {
		int ret = tfs_load_image((char *)&__binary_ramdisk_cpio_start);
		if (ret < 0) {
			error("[CXLFS] Failed to populate fresh image: %d\n", ret);
			return ret;
		}
	} else {
		if (cxlfs_restore_tree(tmpfs_root) < 0)
			return -1;
		/* A normal restart also consumes any untruncated redo tail. */
		plog_replay(local_plog);
	}
	int verify_ret = verify_cxlfs_boot_data(
		(char *)&__binary_ramdisk_cpio_start);
	if (verify_ret < 0) {
		error("[CXLFS] Persistent data verification failed: %d\n",
		      verify_ret);
		return verify_ret;
	}
	cxlfs_boot_data_validated = 1;
	info("[CXLFS] mount verified: owner=%d generation=%lu root=%lu "
	     "data=libc.so-ok checksum=%lx fresh=%d\n",
	     cxlfs_machine(), (unsigned long)cxlfs_generation(),
	     (unsigned long)cxlfs_root_ino(),
	     (unsigned long)cxlfs_boot_data_checksum, fresh);
	if (plog_checkpoint() == 0)
		plog_truncate();

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
	printf("[TIMING] fs_startup: %ld ms\n", timespec_diff_ms(&fs_t0, &fs_t1));

	usys_exit(0);
	return 0;
}
