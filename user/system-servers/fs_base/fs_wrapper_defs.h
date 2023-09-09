#pragma once

#include <chcore/container/list.h>
#include <chcore-internal/fs_defs.h>
#include <chcore/ipc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <malloc.h>
#include <string.h>

/* +++++++++++++++++++++ FS Server Private Data +++++++++++++++++++++++++++ */
#define TEST_COUNT_PAGE true
#if TEST_COUNT_PAGE == 1
#define TEST_COUNT_PAGE_CACHE
#endif
#ifdef TEST_COUNT_PAGE_CACHE
struct test_count {
	int hit;
	int miss;
	int disk_i;
	int disk_o;
};
extern struct test_count count;
#endif

/* Indicates whether a certain fs has been mounted */
extern bool mounted;
extern bool using_page_cache;
extern struct fs_server_ops server_ops;


/* +++++++++++++++++++++++++++ FID Mapping ++++++++++++++++++++++++++++++++ */

#define MAX_SERVER_ENTRY_PER_CLIENT     1024
/* (client_badge, fd) -> fid(server_entry) */
struct server_entry_node {
	u64 client_badge;
	int fd_to_fid[MAX_SERVER_ENTRY_PER_CLIENT]; // TODO: server_entry instead

	struct list_head node;
};

extern struct list_head server_entry_mapping;

void init_fs_wrapper(void);
int fs_wrapper_get_server_entry(u64 client_badge, int fd);
void fs_wrapper_set_server_entry(u64 client_badge, int fd, int fid);
void translate_fd_to_fid(u64 client_badge, struct fs_request *fr);

/* ++++++++++++++++++++++++ FS Server Operations ++++++++++++++++++++++++++ */

/**
 * FS Server Operation Vector
 *
 * NOTE:
 * Each fs server should implement its own operations,
 * 	and store in `server_ops` variable.
 * If there is any need to expand this structure,
 * 	do not forget add a default operation for every fs server impl.
 */
struct fs_server_ops {
	int (*mount) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*umount) (ipc_msg_t *ipc_msg, struct fs_request *fr);

	int (*open) (char *path, int flags, int mode, ino_t *vnode_id, size_t *vnode_size, int *vnode_type, void **private);
	ssize_t (*read) (void *operator, off_t offset, size_t size, char *buf);
	ssize_t (*write) (void *operator, off_t offset, size_t size, const char *buf);
	int (*close) (void *operator, bool is_dir);

	int (*creat) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*unlink) (const char *path, int flags);
	int (*mkdir) (const char *path, mode_t mode);
	int (*rmdir) (const char *path, int flags);
	int (*rename) (const char *oldpath, const char *newpath);

	int (*getdents64) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*ftruncate) (void *operator, size_t size);
	int (*fstatat) (const char *, struct stat *st, int flags);
	int (*fstat) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*statfs) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*fstatfs) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*faccessat) (ipc_msg_t *ipc_msg, struct fs_request *fr);

	int (*symlinkat) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	ssize_t (*readlinkat) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*fallocate) (ipc_msg_t *ipc_msg, struct fs_request *fr);
	int (*fcntl) (void *operator, int fd, int fcntl_cmd, int fcntl_arg);

#ifdef CHCORE_ENABLE_FMAP
	vaddr_t (*fmap_get_page_addr)(void *operator, size_t offset);
#endif
};

/* TODO: uniform default operation, or use NULL instead */
int default_server_operation(ipc_msg_t *ipc_msg, struct fs_request *fr);
ssize_t default_ssize_t_server_operation(ipc_msg_t *ipc_msg, struct fs_request *fr);
#ifdef CHCORE_ENABLE_FMAP
#define default_fmap_get_page_addr NULL
int fs_wrapper_fmap(u64 client_badge, ipc_msg_t *ipc_msg, struct fs_request *fr, bool *ret_with_cap);
#endif
int fs_wrapper_open(u64 client_badge, ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_close(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_read(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_pread(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_write(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_lseek(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_ftruncate(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_fstatat(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_unlink(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_rename(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_count(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_rmdir(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_mkdir(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_sync(void);
int fs_wrapper_fsync(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_creat(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_getdents64(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_fstat(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_statfs(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_fstatfs(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_faccessat(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_symlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_readlinkat(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_fallocate(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_fcntl(u64 client_badge, ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_mount(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_wrapper_umount(ipc_msg_t *ipc_msg, struct fs_request *fr);
int fs_finish_fork(ipc_msg_t *ipc_msg, u64 child_badge, u64 parent_badge);

void fs_server_dispatch(ipc_msg_t *ipc_msg, u64 client_badge);

/* ++++++++++++++++++++++++ Concurrency Control ++++++++++++++++++++++++++ */

extern pthread_rwlock_t fs_wrapper_meta_rwlock;
