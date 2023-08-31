#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This file will be used in both `fsm`, client, server */

#define AT_FDROOT (-101)
#define FS_REQ_PATH_BUF_LEN (256)
#define FS_REQ_PATH_LEN (255)
#define FS_BUF_SIZE (IPC_SHM_AVAILABLE - sizeof(struct fs_request))

/* IPC request type for fs */
enum fs_req_type {
	FS_REQ_UNDEFINED = 0,

	FS_REQ_OPEN,
	FS_REQ_CLOSE,

	FS_REQ_CREAT,
	FS_REQ_MKDIR,
	FS_REQ_RMDIR,
	FS_REQ_SYMLINKAT,
	FS_REQ_UNLINK,
	FS_REQ_RENAME,
	FS_REQ_READLINKAT,

	FS_REQ_READ,
	FS_REQ_WRITE,
	FS_REQ_PREAD,
	FS_REQ_PWRITE,

	FS_REQ_FSTAT,
	FS_REQ_FSTATAT,
	FS_REQ_STATFS,
	FS_REQ_FSTATFS,

	FS_REQ_LSEEK,
	FS_REQ_GETDENTS64,

	FS_REQ_FTRUNCATE,
	FS_REQ_FALLOCATE,

	FS_REQ_FACCESSAT,

	FS_REQ_FCNTL,

#ifdef CHCORE_ENABLE_FMAP
	FS_REQ_FMAP, /* The first phase of mmap. */
#endif

	FS_REQ_MOUNT,
	FS_REQ_UMOUNT,

	FS_REQ_SYNC,
	FS_REQ_FSYNC,
	FS_REQ_FDATASYNC,

	FS_REQ_TEST_PERF, /* Test the page cache miss/hit count，disk I/O count  . */
	
	FS_CHILD_FINISH_FORK,
	
	FS_REQ_MAX

};

/* Client send fsm_req to FSM */
enum fsm_req_type {
	FSM_REQ_UNDEFINED = 0,

	FSM_REQ_PARSE_PATH,
	FSM_REQ_MOUNT,
	FSM_REQ_UMOUNT,

	FSM_REQ_SYNC,
	/*
	 * Since procmgr is booted after fsm and fsm needs to send IPCs to procmgr,
	 * procmgr will issue the following IPC to connect itself with fsm.
	 */
	FSM_REQ_CONNECT_PROCMGR_AND_FSM,

	FSM_CHILD_FINISH_FORK
};

#define FS_READ_BUF_SIZE (IPC_SHM_AVAILABLE - (u64)(&(((fs_request *)(0))->read_buff_begin)))
#define FS_WRITE_BUF_SIZE (IPC_SHM_AVAILABLE - (u64)(&(((fs_request *)(0))->write->write_buff_begin)))

/* Clients send fs_request to fs_server */
struct fs_request {
	enum fs_req_type req;
	union {
		struct {
			int paritition;
			off_t offset;
		} mount;
		struct {
			int new_fd;
			char pathname[FS_REQ_PATH_BUF_LEN];
			int flags;
			mode_t mode;
			int fid;
		} open;
		struct {
			char pathname[FS_REQ_PATH_BUF_LEN];
			mode_t mode;
		} creat;
		struct {
			int fd;
		} close;
		struct {
			int fd;
			size_t count;
		} read;
		struct {
			int fd;
			size_t count;
			char write_buff_begin;
		} write;
		struct {
			int fd;
			size_t count;
			off_t offset;
		} pread;
		struct {
			int fd;
			off_t offset;
			int whence;
		} lseek;
		struct {
			char pathname[FS_REQ_PATH_BUF_LEN];
			mode_t mode;
		} mkdir;
		struct {
			int fd;
			off_t length;
		} ftruncate;
		struct {
			int fd;
			int dirfd;
			char pathname[FS_REQ_PATH_BUF_LEN];
			int flags;
		} stat;
		struct {
			int fd;
			mode_t mode;
			off_t offset;
			off_t len;
		} fallocate;
		struct {
			int fd;
			int fcntl_cmd;
			int fcntl_arg;
		} fcntl;
		struct {
			char pathname[FS_REQ_PATH_BUF_LEN];
			int flags;
		} unlink;
		struct {
			char pathname[FS_REQ_PATH_BUF_LEN];
			int flags;
		} rmdir;
		struct {
			char oldpath[FS_REQ_PATH_BUF_LEN];
			char newpath[FS_REQ_PATH_BUF_LEN];
		} rename;
		struct {
			int fd;
		} fsync;
		struct {
			int fd;
		} fdatasync;
#ifdef CHCORE_ENABLE_FMAP
		struct {
			void *addr;
			size_t length;
			int prot;
			int flags;
			int fd;
			off_t offset;
		} mmap;
#endif
		struct {
			int fd;
			size_t count;
		} getdents64;
		struct {
			char pathname[FS_REQ_PATH_BUF_LEN];
			mode_t mode;
			int flags;
		} faccessat;
		struct {
			char target[FS_REQ_PATH_BUF_LEN];
			char linkpath[FS_REQ_PATH_BUF_LEN];
		} symlinkat;
		struct {
			char pathname[FS_REQ_PATH_BUF_LEN];
			char buf[FS_REQ_PATH_BUF_LEN];
			size_t bufsiz;
		} readlinkat;
		struct {
			unsigned long parentBagde; // for finish_fork
			unsigned long childBadge;
		} fork;
        };
};

struct fsm_request {
	/* Request Type */
	enum fsm_req_type req;

	/* Arguments */
	// Means `path to parse` in normal cases. `device_name` for FSM_REQ_MOUNT/UMOUNT
	char path[FS_REQ_PATH_BUF_LEN];
	int path_len;

	/* Arguments or Response */
	// As arguements when FSM_REQ_MOUNT/UMOUNT, as reponse when FSM_REQ_PARSE_PATH
	char mount_path[FS_REQ_PATH_BUF_LEN];
	int mount_path_len;

	/* Response */
	int mount_id;
	int new_cap_flag;
	
	unsigned long parentBagde; // for finish_fork
};


#ifdef __cplusplus
}
#endif
