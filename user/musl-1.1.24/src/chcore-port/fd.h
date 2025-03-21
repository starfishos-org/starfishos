#pragma once

#include "chcore/container/hashtable.h"
#include "chcore/ipc.h"
#include <termios.h>
#include <chcore-internal/lwip_defs.h>

#include "poll.h"
#include "fs_client_defs.h"

#define MAX_FD  1024
#define MIN_FD  0

#define warn(fmt, ...) printf("[WARN] " fmt, ##__VA_ARGS__)
/* Type of fd */
enum fd_type {
	FD_TYPE_FILE = 0,
	FD_TYPE_PIPE,
	FD_TYPE_SOCK,
	FD_TYPE_STDIN,
	FD_TYPE_STDOUT,
	FD_TYPE_STDERR,
	FD_TYPE_EVENT,
	FD_TYPE_TIMER,
	FD_TYPE_EPOLL,
	FD_TYPE_DEV,
};

struct fd_ops {
	int (*read) (int fd, void *buf, size_t count);
	int (*write) (int fd, void *buf, size_t count);
	int (*close) (int fd);
	int (*poll) (int fd, struct pollarg *arg);
	int (*ioctl) (int fd, unsigned long request, void *arg);
	int (*fcntl) (int fd, int cmd, int arg);
};

extern struct fd_ops epoll_ops;
extern struct fd_ops socket_ops;
extern struct fd_ops file_ops;
extern struct fd_ops event_op;
extern struct fd_ops timer_op;
extern struct fd_ops pipe_op;
extern struct fd_ops stdin_ops;
extern struct fd_ops stdout_ops;
extern struct fd_ops stderr_ops;

/*
 * Each fd will have a fd structure `fd_desc` which can be found from
 * the `fd_dic`. `fd_desc` structure contains the basic information of
 * the fd.
 */
struct fd_desc {
	/* Identification used by corresponding service */
	union {
		int conn_id;
		int fd;
	};
	/* Baisc informantion of fd */
	int flags;		/* Flags of the file */
	int cap;		/* Service's cap of fd, 0 if no service */
	int subcap;		/* A sub services cap, usually used for bypass the centralized service manager.  */
	int rd_ringbuf_cap;	/* Cap of ring buffer to read from. */
	int wr_ringbuf_cap;	/* Cap of ring buffer to write to . */
	ipc_struct_t *subcap_ipc_struct; /* Used for IPC via subcap. */
	void *rd_ringbuf;	/* Ring buffer to transfer huge data from. */
	void *wr_ringbuf;	/* Ring buffer to transfer huge data to. */
	enum fd_type type;	/* Type for debug use */
	struct fd_ops *fd_op;

	/* stored termios */
	struct termios termios;

	/* Private data of fd */
	void *private_data;
};

extern struct fd_desc *fd_dic[MAX_FD];

/* fd */
int alloc_fd(void);
int alloc_fd_since(int min);
void free_fd(int fd);

/* fd operation */
int chcore_read(int fd, void *buf, size_t count);
int chcore_write(int fd, void *buf, size_t count);
int chcore_close(int fd);
int chcore_ioctl(int fd, unsigned long request, void *arg);
int chcore_readv(int fd, const struct iovec *iov, int iovcnt);
int chcore_writev(int fd, const struct iovec *iov, int iovcnt);
int dup_fd_content(int fd, int arg);
long chcore_fd_mmap(long vaddr, size_t length, int prot, int flags, int fd, off_t offset);
