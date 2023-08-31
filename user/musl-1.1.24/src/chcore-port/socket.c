#include <pthread.h>
#include <stdarg.h>
#include <stdnoreturn.h>
#include <sys/ioctl.h>
#include <syscall_arch.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

/* ChCore header */
#include <chcore/ipc.h>
#include <chcore-internal/lwip_defs.h>
#include <chcore/syscall.h>
#include <chcore/memory.h>

/* ChCore port header */
#include "fd.h"

#define NO_SUBCAP (-1)
#define PASSIVE_SUBCAP (-2)  // Speical SUBCAP value indicating this is a server
                             // that passively receives data.

#define debug(...) {} 
// #define debug(...) printf(__VA_ARGS__)

/* Helper function */
#define lwip_ipc(x...) __lwip_ipc(NULL, x)
#define lwip_ipc_cap(cap, x...) __lwip_ipc_cap(NULL, cap, x)
#define __lwip_ipc(ipc_msg_p, x...) __lwip_ipc_cap(ipc_msg_p, 0, x)

/* Use __lwip_ipc when need to get message after IPC return */
static int __lwip_ipc_cap(ipc_msg_t **ipc_msg_p, u32 cap, enum LWIP_REQ req, void *data,
		      size_t data_size, int nr_args, ...)
{
	struct lwip_request *lr_ptr;
	ipc_msg_t *ipc_msg;
	va_list args;
	int ret = 0, i = 0;

	ipc_struct_t *str = lwip_ipc_struct;
	ipc_msg =
		ipc_create_msg(str, sizeof(struct lwip_request), cap ? 1 : 0);
	if (cap) {
		ipc_set_msg_cap(ipc_msg, 0, cap);
	}
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	lr_ptr->req = req;

	va_start(args, nr_args);
	for (i = 0; i < nr_args; ++i)
		lr_ptr->args[i] = va_arg(args, unsigned long);
	va_end(args);
	if (data) {
		BUG_ON(data_size > LWIP_DATA_LEN);
		memcpy(lr_ptr->data, data, data_size);
	}
	ret = ipc_call(lwip_ipc_struct, ipc_msg);

	if (ipc_msg_p)
		*ipc_msg_p = ipc_msg;
	else
		ipc_destroy_msg(ipc_msg);
	return ret;
}

struct ringbuf_header {
	char is_delayed_ringbuf;
	int pos_writer;
	int pos_reader;
	int pos_visiable_writer;
	pthread_mutex_t lock;
};
#define SHARED_RINGBUF_SIZE (1ULL << 20) // 2^4 = 16 KB
#define SHARED_RINGBUF_DATA_SIZE \
	(SHARED_RINGBUF_SIZE - sizeof(struct ringbuf_header))

static int
local_socket_connect(int fd, int server_conn_id)
{
	ipc_struct_t *ipc_struct = fd_dic[fd]->subcap_ipc_struct;
	struct lwip_request *lr_ptr;
	ipc_msg_t *ipc_msg;
	int ret = 0, i = 0;
	struct ringbuf_header *header;
	// 2MB shared ring buffer
	int ringbuf_cap = usys_create_pmo(SHARED_RINGBUF_SIZE, PMO_RING_BUFFER);
	if (ringbuf_cap < 0) {
		printf("usys_create_pmo ret %d\n", ringbuf_cap);
		usys_exit(-1);
	}
	fd_dic[fd]->wr_ringbuf_cap = ringbuf_cap;
	fd_dic[fd]->wr_ringbuf = chcore_auto_map_pmo(ringbuf_cap,
						     SHARED_RINGBUF_SIZE,
						     VM_READ | VM_WRITE);
	header = fd_dic[fd]->wr_ringbuf;
	pthread_mutex_init(&(header->lock), 0);
	header->is_delayed_ringbuf = 0;
	header->pos_writer = 0;
	header->pos_reader = 0;
	ringbuf_cap = usys_create_pmo(SHARED_RINGBUF_SIZE, PMO_RING_BUFFER);
	if (ringbuf_cap < 0) {
		printf("usys_create_pmo ret %d\n", ringbuf_cap);
		usys_exit(-1);
	}
	fd_dic[fd]->rd_ringbuf_cap = ringbuf_cap;
	fd_dic[fd]->rd_ringbuf = chcore_auto_map_pmo(ringbuf_cap,
						     SHARED_RINGBUF_SIZE,
						     VM_READ | VM_WRITE);
	header = fd_dic[fd]->rd_ringbuf;
	pthread_mutex_init(&(header->lock), 0);
	header->pos_writer = 0;
	header->pos_reader = 0;
	/* client's rd_ringbuf is a delayed ringbuf */
	/* this will be set when register */
	header->is_delayed_ringbuf = 0;

	ipc_msg = ipc_create_msg(ipc_struct,
				 sizeof(struct lwip_request),
				 2);
	ipc_set_msg_cap(ipc_msg, 0, fd_dic[fd]->wr_ringbuf_cap);
	ipc_set_msg_cap(ipc_msg, 1, fd_dic[fd]->rd_ringbuf_cap);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	lr_ptr->req = LWIP_LOCAL_SOCKET_FORWARD_CONNECT;

	// Use the server_conn_id as the token. The server_conn_id is
	//   received from the server during connect.
	// The server will use the conn_id to reversely find the fd.
	lr_ptr->args[0] = server_conn_id;

	debug("TCP client try to connect with TCP server's IPC server: conn_id=%d\n", server_conn_id);
	ret = ipc_call(ipc_struct, ipc_msg);
	ipc_destroy_msg(ipc_msg);
	return ret;
}

// The ringbuf rules:
//   The ringbuf is just a memory area of size `SHARED_RINGBUF_SIZE`.
//   The first part of the area is `struct ringbuf_header`, usually
//     two integers: pos_writer and pos_reader.
//
//      +-------------+
//      |             |
//   +----------------v-------------------------------------+
//   |W|R|            xxxxx                                 |
//   +---------------------^--------------------------------+
//    |                    |
//    +--------------------+
//
//   The value of pos_xxx should >= 0, and <
//     SHARED_RINGBUF_SIZE - sizeof(struct ringbuf_header), aka
//     SHARED_RINGBUF_DATA_SIZE
//
static inline
int ringbuf_to_end(int pos)
{
	return SHARED_RINGBUF_DATA_SIZE - pos;
}
static inline
int ringbuf_data_size(struct ringbuf_header *header)
{
	int size = header->pos_writer - header->pos_reader;
	if (size < 0) { 
		debug("ringbuf_data_size: wpos=%d, rpos=%d\n", 
			header->pos_writer, header->pos_reader);
		size += SHARED_RINGBUF_DATA_SIZE;
	}
	return size;
}
static inline
int ringbuf_visiable_data_size(struct ringbuf_header *header)
{
	int size =header->pos_visiable_writer - header->pos_reader;
	if (size < 0) size += SHARED_RINGBUF_DATA_SIZE;
		return size;
}
static inline
int ringbuf_free_size(struct ringbuf_header *header)
{
	return SHARED_RINGBUF_DATA_SIZE - ringbuf_data_size(header);
}

static int local_socket_write(int fd, void *buf, size_t count)
{
	struct ringbuf_header *header = fd_dic[fd]->wr_ringbuf;
	char *ringbuf_data = (char *)header + sizeof(*header);

	// debug("%s: fd=%d buf=x count=%d\n", __func__, fd, count);
	// pthread_mutex_lock(&header->lock);
	// TODO: Do we allow partial write?
	if (count > ringbuf_free_size(header)) {
		// No enough free space
		debug("WRITE FAIL: No enough free space");
		return -1;
	}
	debug("WRITE OK fd=%d pos: wr=%d rd=%d count=%d\n", //bytes=%4s\n",
	       fd, header->pos_writer, header->pos_reader, count);
	    //    ((char *)(buf)));
	// How much space do we have till the end of ringbuf data area
	int to_end = ringbuf_to_end(header->pos_writer);
	if (count <= to_end) {
		// Direct copy is okay
		memcpy(ringbuf_data + header->pos_writer, buf, count);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		header->pos_writer += count;
	} else {
		// Copy in two steps
		memcpy(ringbuf_data + header->pos_writer, buf, to_end);
		memcpy(ringbuf_data, (char *)buf + to_end, count - to_end);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		header->pos_writer += count - SHARED_RINGBUF_DATA_SIZE;
	}
	// pthread_mutex_unlock(&header->lock);
	debug("WRITE OK fd=%d pos: wr=%d rd=%d count=%d\n", //bytes=%4s\n",
	       fd, header->pos_writer, header->pos_reader, count);
	    //    ((char *)(buf)));
	return count;
}

static int local_socket_writev(int fd, struct iovec *iov, size_t iovlen)
{
	int i;
	int count = 0;
	for (i = 0; i < iovlen; ++i) {
		int ret = -1;
		while (ret == -1) {
			ret = local_socket_write(fd,
						 iov[i].iov_base,
						 iov[i].iov_len);
		}
		count += iov[i].iov_len;
	}
	return count;
}

static int __local_socket_read(int fd, void *buf, size_t count)
{
	struct ringbuf_header *header = fd_dic[fd]->rd_ringbuf;
	char *ringbuf_data = (char *)header + sizeof(*header);
	int datasize;; 
	
	// pthread_mutex_lock(&header->lock);
	if (header->is_delayed_ringbuf) 
		datasize = ringbuf_visiable_data_size(header);
	else 
		datasize = ringbuf_data_size(header);
	if (datasize == 0) {
		// No enough data availabe
		// debug("no enough data availabe\n");
		// pthread_mutex_unlock(&header->lock);
		return -1;
	}
	// debug("datasize=%d, count=%d\n", datasize, count);
	if (datasize < count)
		count = datasize;
	// How much data do we have till the end of ringbuf data area
	int to_end = ringbuf_to_end(header->pos_reader);
	if (count <= to_end) {
		// Direct copy is okay
		memcpy(buf, ringbuf_data + header->pos_reader, count);
		// FIXME: Do we need barriers?
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		header->pos_reader += count;
	} else {
		// Copy in two steps
		memcpy(buf, ringbuf_data + header->pos_reader, to_end);
		memcpy((char *)buf + to_end, ringbuf_data, count - to_end);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		header->pos_reader += count - SHARED_RINGBUF_DATA_SIZE;
	}
	// pthread_mutex_unlock(&header->lock);
	debug("READ OK: fd=%d pos: wr=%d rd=%d count=%d\n", 
		fd, header->pos_writer, header->pos_reader, count);
	// TODO: Do we allow partial read?
	return count;
}

static int local_socket_read(int fd, void *buf, size_t count)
{
	int ret;
	// debug("%s: fd=%d buf=x count=%d\n", __func__, fd, count);
	for (;;) {
		ret = __local_socket_read(fd, buf, count);
		if (ret != -1) break;
		// printf("Yield for later attempt to read\n");
		// FIXME: change to nop?
		// usys_yield();
		// printf("%s: loop\n", __func__);
		// usys_top();
	}
	// debug("READ OK: fd=%d buf=x ret=%d\n", fd, ret);
	return ret;
}

static int local_socket_readv(int fd, struct iovec *iov, size_t iovlen)
{
	int i;
	int count = 0;
	for (i = 0; i < iovlen; ++i) {
		int ret = -1;
		int sum = 0;
		while (sum != iov[i].iov_len) {
			ret = local_socket_read(fd,
						(char *)iov[i].iov_base + sum,
						iov[i].iov_len - sum);
			if (ret == -1)
				continue;
			sum += ret;
		}
		count += iov[i].iov_len;
	}
	return count;
}

/* ChCore socket fd operation (with socket prefix) */

static int chcore_socket_read(int fd, void *buf, size_t count)
{
	ipc_msg_t *ipc_msg;
	struct lwip_request *lr_ptr = 0;
	int ret = 0, len = 0, remain = 0, bias = 0;
	ipc_struct_t *target = lwip_ipc_struct;

	/* fd has already been checked */
	if (count != 0 && buf == NULL)
		return -EFAULT;

	// Local Socket Fowarding
	if (fd_dic[fd]->subcap != NO_SUBCAP) {
		return local_socket_read(fd, buf, count);
	}

	ipc_msg =
	    ipc_create_msg(lwip_ipc_struct, sizeof(struct lwip_request), 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	lr_ptr->args[0] = fd_dic[fd]->conn_id;
	/* If data is too large, seperate to multiple IPC */
	if (count > LWIP_DATA_LEN) {
		remain = count;
		ret = len = 0;
		// FIXME: (MK) is this a bug?
		lr_ptr->req = LWIP_SOCKET_RECV; /* We need to use flags */
		lr_ptr->args[2] = 0;		/* flags */
		lr_ptr->args[3] = 0;		/* alen */
		while (remain > 0 && ret == len) {
			len = remain > LWIP_DATA_LEN ? LWIP_DATA_LEN : remain;
			lr_ptr->args[1] = len;
			if ((ret = ipc_call(target, ipc_msg)) < 0) {
				ret = bias > 0 ? bias : ret;
				goto out;
			}
			BUG_ON(ret > LWIP_DATA_LEN);
			memcpy((void *)((char *)buf + bias), lr_ptr->data, ret);
			remain -= ret;
			bias += ret;
			lr_ptr->args[2] |= MSG_DONTWAIT;
		}
		ret = bias;
	} else { /* else one single ipc is enough */
		lr_ptr->req = LWIP_SOCKET_READ;
		lr_ptr->args[1] = count; /* len */
		if ((ret = ipc_call(target, ipc_msg)) < 0)
			goto out;
		BUG_ON(ret > LWIP_DATA_LEN);
		memcpy(buf, lr_ptr->data, ret);
	}
out:
	ipc_destroy_msg(ipc_msg);
	return ret;
}

static int chcore_socket_write(int fd, void *buf, size_t count)
{
	ipc_msg_t *ipc_msg;
	struct lwip_request *lr_ptr = 0;
	int ret = 0;
	int len = 0, remain = 0, bias = 0;
	ipc_struct_t *target = lwip_ipc_struct;

	if (count != 0 && buf == NULL)
		return -EFAULT;

	// Local Socket Fowarding
	if (fd_dic[fd]->subcap != NO_SUBCAP) {
		return local_socket_write(fd, buf, count);
	}

	ipc_msg =
	    ipc_create_msg(target, sizeof(struct lwip_request), 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	lr_ptr->req = LWIP_SOCKET_WRITE;
	/* Find conn_id from fd_desc */
	lr_ptr->args[0] = fd_dic[fd]->conn_id;

	if (count > LWIP_DATA_LEN) {
		/* If data is too large, seperate to multiple IPC */
		remain = count;
		len = ret = 0;
		while (remain > 0 && ret == len) {
			/* If remain cannot send in one ipc, leave it to the
			 * next ipc */
			len = remain > LWIP_DATA_LEN ? LWIP_DATA_LEN : remain;
			memcpy(lr_ptr->data, (void *)((char *)buf + bias), len);
			lr_ptr->args[1] = len; /* len */
			if ((ret = ipc_call(target, ipc_msg)) < 0) {
				ret = bias > 0 ? bias : ret;
				goto out;
			}
			bias += ret;
			remain -= ret;
		}
		ret = bias;
	} else {
		/* Else one single ipc is enough */
		memcpy(lr_ptr->data, buf, count);
		lr_ptr->args[1] = count; /* len */
		ret = ipc_call(target, ipc_msg);
	}
out:
	ipc_destroy_msg(ipc_msg);
	return ret;
}

static int chcore_socket_close(int fd)
{
	ipc_msg_t *ipc_msg;
	struct lwip_request lr = {0};
	int ret;

	lr.req = LWIP_SOCKET_CLSE;
	lr.args[0] = fd_dic[fd]->conn_id;
	ret = simple_ipc_forward(lwip_ipc_struct, &lr, sizeof(lr));

	// TODO: clean up local socket forwarding.

	if (ret < 0)
		return ret;
	free_fd(fd);
	return ret;
}

// Check whether the given fd is
//   -1: invalid socket fd
//   0: remote socket fd
//   1: local socket fd
int local_socket_check_fd(int fd)
{
	if (fd >= MAX_FD || fd < 0) // Invalid fd
		return -1;
	if (fd_dic[fd] == NULL) // ? why ?
		return -1;
	if (fd_dic[fd]->type != FD_TYPE_SOCK)
		return -1;
	if (fd_dic[fd]->cap != lwip_server_cap)
		return -1;
	if (fd_dic[fd]->subcap == NO_SUBCAP)
		return 0;
	return 1;
}

int local_socket_poll(struct pollfd fds[], nfds_t nfds, int timeout,
		      bool update_fds)
{
	int nready = 0;
	int i;
	// printf("local socket polling\n");
	for (i = 0; i < nfds; i++) {
		fds[i].revents = 0;
		int fd = fds[i].fd;
		int type = local_socket_check_fd(fd);
		if (type != 1)
			continue;

		if ((fds[i].events & POLLIN) &&
		    ringbuf_data_size((struct ringbuf_header *)
				      fd_dic[fd]->rd_ringbuf))
			fds[i].revents |= POLLIN;
		if ((fds[i].events & POLLOUT) &&
		    ringbuf_free_size((struct ringbuf_header *)
				      fd_dic[fd]->wr_ringbuf))
			fds[i].revents |= POLLOUT;
		if (fds[i].revents)
			nready += 1;
	}
	return nready;
}

int local_socket_epoll(struct eventpoll *ep, int timeout, struct epoll_event *events)
{
	struct epitem *epi = NULL, *tmp = NULL;
	int local_nready = 0;
	int fd, type;
	short event, revent;

	for_each_in_list_safe(epi, tmp, epi_node, &ep->epi_list) {  
		fd = epi->fd;
		type = local_socket_check_fd(fd);
		event = epi->event.events & ~EPOLLEXCLUSIVE &
			~EPOLLWAKEUP & ~EPOLLONESHOT & ~EPOLLET;
		revent = 0;
		
		if (type != 1)
			continue;

		if ((event & POLLIN) &&
			ringbuf_data_size((struct ringbuf_header *)
					fd_dic[fd]->rd_ringbuf))
			revent |= POLLIN;
		if ((event & POLLOUT) &&
			ringbuf_free_size((struct ringbuf_header *)
					fd_dic[fd]->wr_ringbuf))
			revent |= POLLOUT;
		if (revent) {
			events[local_nready].events = revent;
			events[local_nready].data = epi->event.data;
			local_nready += 1;
		}
	}

	return local_nready;
}

static int __chcore_socket_poll(struct pollfd fds[], nfds_t nfds, int timeout,
				bool update_fds)
{
	int i, ret;
	struct pollfd *fd_iter, *server_fd_iter;
	ipc_msg_t *ipc_msg;
	struct lwip_request *lr_ptr = 0;

	if (fds == 0)
		return -EFAULT;

	if (nfds == 0)
		return 0;

	if (nfds * (sizeof(struct pollfd)) > LWIP_DATA_LEN) {
		WARN("IPC message is not large enough to store the poll fds\n");
		return -ENOMEM;
	}

	// Check for local socket fowarding.
	// NOTICE: all the polling fd should be either all or none local socket
	bool found_remote_socket = false;
	bool found_local_socket = false;
	for (i = 0; i < nfds; i++) {
		int fd = fds[i].fd;
		int type = local_socket_check_fd(fd);
		if (type == -1) // Invalid socket fd
			continue;
		if (type == 0)
			found_remote_socket = true;
		else
			found_local_socket = true;
	}
	if (found_remote_socket && found_local_socket) {
		// FIXME: We CANNOT handle this for now.
		printf("We do not support mixed socket polling\n");
		BUG_ON(1);
	}
	if (found_local_socket) {
		// Local Socket Forwarding
		return local_socket_poll(fds, nfds, timeout, update_fds);
	}

	ipc_msg =
	    ipc_create_msg(lwip_ipc_struct, sizeof(struct lwip_request), 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);

	lr_ptr->req = LWIP_REQ_SOCKET_POLL;
	lr_ptr->args[1] = nfds;
	lr_ptr->args[2] = timeout;

	for (i = 0; i < nfds; i++) {
		server_fd_iter = ((struct pollfd *)lr_ptr->data) + i;
		/* Check fd here, only passing sock to server */
		if (fds[i].fd < MAX_FD && fds[i].fd >= 0 && /* valid fd */
		    fd_dic[fds[i].fd] != NULL &&
		    fd_dic[fds[i].fd]->type == FD_TYPE_SOCK) {
			server_fd_iter->fd = fd_dic[fds[i].fd]->conn_id;
			server_fd_iter->events = fds[i].events;
		} else {
			server_fd_iter->fd = -1;
		}
	}

	ret = ipc_call(lwip_ipc_struct, ipc_msg);

	if (update_fds) {
		/* Copy the return value */
		for (i = 0; i < nfds; i++) {
			fd_iter = fds + i;
			server_fd_iter = ((struct pollfd *)lr_ptr->data) + i;
			fd_iter->revents = server_fd_iter->revents;
		}
	}

	ipc_destroy_msg(ipc_msg);
	return ret;
}

static int chcore_socket_poll(int fd, struct pollarg *arg)
{
#if 0 /* Poll every time */
	struct pollfd pfd;

	/* only check one fd */
	pfd.fd = fd;
	pfd.events = arg->events;
	pfd.revents = 0;
	/* Return immediately */
	return __chcore_socket_poll(&pfd, 1, 0);
#else /* Poll afterwards */
	return 0;
#endif
}

struct socket_poll_arg {
	struct pollfd *fds;
	nfds_t nfds;
	int timeout;
	int notifc_cap;
};

static void *socket_poll_thread_routine(void *arg)
{
	struct socket_poll_arg *spa = (struct socket_poll_arg *)arg;
	int ret = 0;

	ret = __chcore_socket_poll(spa->fds, spa->nfds, spa->timeout, false);
	/* Check return value */
	if (ret >= 0) /* if ret = 0, the timer of lwip expired */
		//chcore_syscall1(CHCORE_SYS_notify, spa->notifc_cap);
		usys_notify(spa->notifc_cap);
	free(arg);
	return NULL;
}

/*
 * If timeout == 0, direclty poll once and return immediately
 * Else, create a seperate thread to poll and notify the notifc afterwards.
 * Return count if timeout = 0, return errno if timeout > 0
 */
int chcore_socket_server_poll(struct pollfd fds[], nfds_t nfds, int timeout,
			      int notifc_cap)
{
	/* Create a thread and call poll function */
	pthread_t tid;
	struct socket_poll_arg *arg;
	int ret;

	if (timeout == 0) {
		ret = __chcore_socket_poll(fds, nfds, 0, true);
	} else {
		/* Prepare arg */
		arg = malloc(sizeof(struct socket_poll_arg));
		if (arg <= 0)
			return -ENOMEM;
		arg->fds = fds;
		arg->nfds = nfds;
		arg->timeout = timeout;
		arg->notifc_cap = notifc_cap;
		ret =
		    pthread_create(&tid, NULL, socket_poll_thread_routine, arg);
	}
	return ret;
}

static int chcore_socket_ioctl(int fd, unsigned long request, void *arg)
{
	ipc_msg_t *ipc_msg;
	struct lwip_request lr = {0};
	struct lwip_request *lr_ptr = 0;
	int ret;

	/* Only support several request in lwip */
	if (request == FIONREAD || request == FIONBIO) {
		ret = __lwip_ipc(&ipc_msg, LWIP_SOCKET_IOCTL, arg, sizeof(int),
				 2, fd_dic[fd]->conn_id, request);
		lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
		if (ret == 0)
			/* socket ioctl only return int in lwip */
			memcpy(arg, lr_ptr, sizeof(int));
		ipc_destroy_msg(ipc_msg);
	} else {
		printf("Socket ioctl request %d not supported!\n", request);
		ret = 0;
	}
	return ret;
}

int chcore_socket_fcntl(int fd, int cmd, int arg)
{
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;
	int new_fd, ret = 0;

	switch (cmd) {
	case F_DUPFD_CLOEXEC:
	case F_DUPFD: {
		new_fd = dup_fd_content(fd, arg);
		ipc_msg = ipc_create_msg(
				lwip_ipc_struct, sizeof(struct lwip_request), 0);
		lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
		lr_ptr->req = LWIP_REQ_FCNTL;
		lr_ptr->args[0] = fd_dic[fd]->conn_id; /* flags */
		lr_ptr->args[1] = F_DUPFD; /* alen */
		lr_ptr->args[2] = new_fd;
		ipc_call(lwip_ipc_struct, ipc_msg);
		ipc_destroy_msg(ipc_msg);
		return new_fd;
	}
	case F_GETFL:
	case F_SETFL:
		/* SOCKET fd */
		if (fd_dic[fd]->type == FD_TYPE_SOCK) {
			/*
			 * Remove the O_LARGEFILE bit set in fcntl.c
			 * which is not supported by LWIP.
			 */
			arg &= ~O_LARGEFILE;
			ipc_msg = ipc_create_msg(lwip_ipc_struct,
						 sizeof(struct lwip_request), 0);
			lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
			lr_ptr->req = LWIP_REQ_FCNTL;
			lr_ptr->args[0] = (unsigned long)fd_dic[fd]->conn_id;
			lr_ptr->args[1] = (unsigned long)cmd;
			lr_ptr->args[2] = (unsigned long)arg;

			ret = ipc_call(lwip_ipc_struct, ipc_msg);
			ipc_destroy_msg(ipc_msg);
			return ret;
		}

		warn("does not support F_SETFL for non-fs files\n");
		return -1;

	default:
		return -EINVAL;
	}
	return -1;
}

/* SOCK */
struct fd_ops socket_ops = {
	.read = chcore_socket_read,
	.write = chcore_socket_write,
	.close = chcore_socket_close,
	.poll = chcore_socket_poll,
	.ioctl = chcore_socket_ioctl,
	.fcntl = chcore_socket_fcntl,
};

/* ChCore Socket operation (w/o socket prefix) */

int chcore_socket(int domain, int type, int protocol)
{
	int ret = 0;
	int fd = 0;

	ret = lwip_ipc(LWIP_CREATE_SOCKET, NULL, 0, 3, domain, type, protocol);
	if (ret >= 0) { /* succ */
		/* Allocate fd and fill the table */
		if ((fd = alloc_fd()) < 0)
			return fd;

		fd_dic[fd]->cap = lwip_server_cap;
		fd_dic[fd]->conn_id = ret;
		fd_dic[fd]->type = FD_TYPE_SOCK;
		fd_dic[fd]->fd_op = &socket_ops;
		fd_dic[fd]->subcap = NO_SUBCAP; // Clear the sub cap
		ret = fd;
	}
	return ret;
}

int chcore_setsocketopt(int fd, int level, int optname, const void *optval,
			socklen_t optlen)
{
	if (fd_dic[fd] == 0)
		return -EBADF;
	return lwip_ipc(LWIP_SOCKET_SOPT, optlen ? (void *)optval : NULL,
			optlen, 4, fd_dic[fd]->conn_id, level, optname, optlen);
}

int chcore_getsocketopt(int fd, int level, int optname, void *restrict optval,
			socklen_t *restrict optlen)
{
	int ret = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;

	if (fd_dic[fd] == 0)
		return -EBADF;
	ret = __lwip_ipc(&ipc_msg, LWIP_SOCKET_GOPT, NULL, 0, 4,
			 fd_dic[fd]->conn_id, level, optname,
			 *(unsigned long *)optlen);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	*optlen = lr_ptr->args[3];
	BUG_ON(*optlen > LWIP_DATA_LEN);
	if (optval != 0 && *optlen != 0)
		memcpy((void *)optval, lr_ptr->data, *optlen);
	ipc_destroy_msg(ipc_msg);
	return ret;
}

int chcore_getsockname(int fd, struct sockaddr *restrict addr,
		       socklen_t *restrict len)
{
	int ret = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;

	if (fd_dic[fd] == 0)
		return -EBADF;
	ret = __lwip_ipc(&ipc_msg, LWIP_SOCKET_NAME, NULL, 0, 2,
			 fd_dic[fd]->conn_id, *len);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	*len = lr_ptr->args[1];
	BUG_ON(*len > LWIP_DATA_LEN);
	if (addr != 0 && *len != 0)
		memcpy((void *)addr, lr_ptr->data, *len);
	ipc_destroy_msg(ipc_msg);
	return ret;
}

int chcore_getpeername(int fd, struct sockaddr *restrict addr,
		       socklen_t *restrict len)
{
	int ret = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;

	if (fd_dic[fd] == 0)
		return -EBADF;
	ret = __lwip_ipc(&ipc_msg, LWIP_SOCKET_PEER, NULL, 0, 2,
			 fd_dic[fd]->conn_id, *len);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	*len = lr_ptr->args[1];
	BUG_ON(*len > LWIP_DATA_LEN);
	if (addr != 0 && *len != 0)
		memcpy((void *)addr, lr_ptr->data, *len);
	ipc_destroy_msg(ipc_msg);
	return ret;
}


// cache[X] = Y means conn_id X corresponds to fd Y
// To avoid initializing, Y == 0 means invalid cache
#define MAX_CACHED_CONN_ID (512)
int conn_id_cache[MAX_CACHED_CONN_ID] = {0};
u32 tcp_server_cap = 0;
volatile bool tcp_server_ready = false;

int find_fd_from_conn_id(int conn_id)
{
	// Try to access the cache, which is best effort.
	if (conn_id < MAX_CACHED_CONN_ID) {
		int fd = conn_id_cache[conn_id];
		if (fd)
			return fd;
	}
	for (int i = 0; i < MAX_FD; ++i) {
		if (fd_dic[i] &&
		    fd_dic[i]->type == FD_TYPE_SOCK &&
		    fd_dic[i]->conn_id == conn_id) {
			// Update cache
			if (conn_id < MAX_CACHED_CONN_ID) {
				conn_id_cache[conn_id] = i;
			}
			// Return fd
			return i;
		}
	}
	return -1;
}

void tcp_server_dispatch(ipc_msg_t * ipc_msg, u64 client_badge)
{
	int ret = 0, i = 0;
	int socket = 0, accpet_fd = 0, port = 0, len = 0, flags = 0, backlog =
	    0;
	int cmd = 0, val = 0;
	int domain = 0, protocol = 0, type = 0, level = 0, optname = 0;
	socklen_t alen = 0, optlen = 0, namelen = 0;
	struct msghdr *msg;
	int shared_pmo = 0;
	struct sockaddr target;
	bool has_cap = false;

	if (ipc_msg->data_len < 4) {
		/* No OP NUMBER */
		ret = -EINVAL;
		goto out;
	}

	struct lwip_request *lr =
		(struct lwip_request *)ipc_get_msg_data(ipc_msg);
	int conn_id = lr->args[0];
	int fd = find_fd_from_conn_id(conn_id);
	while (fd < 0) {
		// This msg comes before the lwip server's reply, so that
		//   the fd has not been allocated and initialized.
		//   Just give the other thread a minute.
		fd = find_fd_from_conn_id(conn_id);
	}
	// printf("TCP SErver's IPC server gets conn_id=%d -> fd=%d\n", conn_id, fd);

	switch (lr->req) {
	case LWIP_LOCAL_SOCKET_FORWARD_CONNECT: {
		// The other endpoint's wr is my read.
		int rd_ringbuf_cap = ipc_get_msg_cap(ipc_msg, 0);
		int wr_ringbuf_cap = ipc_get_msg_cap(ipc_msg, 1);
		// We cannot use client_badge to identify client since a
		//   client can have more than one connection to me. Thus,
		//   we have to use conn_id.
		fd_dic[fd]->rd_ringbuf_cap = rd_ringbuf_cap;
		fd_dic[fd]->rd_ringbuf = chcore_auto_map_pmo(rd_ringbuf_cap,
							     SHARED_RINGBUF_SIZE,
							     VM_READ | VM_WRITE);
		fd_dic[fd]->wr_ringbuf_cap = wr_ringbuf_cap;
		fd_dic[fd]->wr_ringbuf = chcore_auto_map_pmo(wr_ringbuf_cap,
							     SHARED_RINGBUF_SIZE,
							     VM_READ | VM_WRITE);
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		// Change subcap at last to indicate the setup is completed.
		fd_dic[fd]->subcap = PASSIVE_SUBCAP;
		// printf("CONN from client, channel established\n");

		/* Now server and client have established the channel, 
		So we register the channel to kernel ckpt server */
		usys_register_external_ringbuf((u64)fd_dic[fd]->wr_ringbuf);
		usys_set_poll_remote();
		ret = 0;
		break;
	}
	case LWIP_SOCKET_RECV:
	case LWIP_SOCKET_RMSG:
	case LWIP_SOCKET_READ: {
		printf("socket: read\n");
		ret = -EINVAL;
		// A receive should loop locally w/o sending IPC to me.
		break;
	}
	case LWIP_SOCKET_SEND:
	case LWIP_SOCKET_SMSG:
	case LWIP_SOCKET_WRITE: {
		printf("socket: write\n");
		// When a client write to me, I should record the msg locally.
		// TODO: We should detech the shared memory in the ipc_msg to
		//   avoid memcpy.
		// TODO: We need an atomic list.
		// fd_dic[fd]->message_list = ;
		// barreir();
		// Notify? It seems that I don't need to notify anyone, since
		//   the readers should be polling if they really need the data.
		break;
	}
	default:
		printf("HELLO req=%d\n", lr->req);
		ret = -EINVAL;
	}

out:
	ipc_return(ipc_msg, ret);
}

static noreturn void *tcp_server_loop(void *arg)
{
	int err;
	// printf("registering ipc server\n");
	err = ipc_register_server(tcp_server_dispatch,
				  DEFAULT_CLIENT_REGISTER_HANDLER);
	tcp_server_ready = true;
	BUG_ON(err);
	while (true) {
		usys_yield();
	}
}

void ensure_tcp_server(void)
{
	assert(!tcp_server_cap);
	pthread_t tid;

	if (tcp_server_cap)
		return;

	tcp_server_ready = false;
	tcp_server_cap = chcore_pthread_create(&tid, NULL,
					       tcp_server_loop, NULL);

	// printf("tcp server cap: %d\n", tcp_server_cap);
	BUG_ON(tcp_server_cap == 0);
	while (!tcp_server_ready) {
		usys_yield();
	}

}

int chcore_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	if (fd_dic[fd] == 0)
		return -EBADF;

	BUG_ON(len > LWIP_DATA_LEN);
	ensure_tcp_server();
	return lwip_ipc_cap(tcp_server_cap, LWIP_SOCKET_BIND, (void *)addr,
			    len, 2, fd_dic[fd]->conn_id, len);
}

int chcore_listen(int fd, int backlog)
{
	if (fd_dic[fd] == 0)
		return -EBADF;
	return lwip_ipc(LWIP_SOCKET_LIST, NULL, 0, 2, fd_dic[fd]->conn_id,
			backlog);
}

int chcore_accept(int fd, struct sockaddr *restrict addr,
		  socklen_t *restrict len)
{
	int ret = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;

	if (fd_dic[fd] == 0)
		return -EBADF;

	ret = __lwip_ipc(&ipc_msg, LWIP_SOCKET_ACPT, NULL, 0, 2,
			 fd_dic[fd]->conn_id, *len);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);

	*len = lr_ptr->args[1];
	BUG_ON(*len > LWIP_DATA_LEN);
	if (addr != 0 && *len != 0)
		memcpy((void *)addr, lr_ptr->data, *len);
	ipc_destroy_msg(ipc_msg);
	if (ret >= 0) { /* succ */
		/* Allocate fd and fill the table */
		if ((fd = alloc_fd()) < 0)
			return fd;

		// printf("tcp server: ACCEPT fd=%d conn_id=%d!\n", fd, ret);
		fd_dic[fd]->subcap = NO_SUBCAP; // Clear the subcap.
		// Barrier here? We need to ensure the subcap is set before
		//   conn_id, so that our change to the subcap won't
		//   be overwritten.
		// FIXME: a looser fence level?
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		fd_dic[fd]->cap = lwip_server_cap;
		fd_dic[fd]->conn_id = ret;
		fd_dic[fd]->type = FD_TYPE_SOCK;
		fd_dic[fd]->fd_op = &socket_ops;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		while (*(volatile int *)(&fd_dic[fd]->subcap) != PASSIVE_SUBCAP) {
			usys_yield();
			// __asm__ __volatile__("nop" ::: "memory");
		}
		ret = fd;
		// printf("tcp server: ACCEPT fd=%d OK!\n", fd);
	}
	return ret;
}

int chcore_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	ipc_msg_t *ipc_msg;
	if (fd_dic[fd] == 0)
		return -EBADF;
	// printf("CONNECTING!!! to %p of cap %d\n", lwip_ipc_struct, lwip_ipc_struct->conn_cap);
	int err = __lwip_ipc(&ipc_msg, LWIP_SOCKET_CONN, (void *)addr, len, 2,
			     fd_dic[fd]->conn_id, len);

	// Local Socket Forwarding:
	// The LWIPServer sends back an cap to the direct TCPServer
	if (!err && ipc_msg->cap_slot_number) {
		BUG_ON(ipc_msg->cap_slot_number != 1);
		fd_dic[fd]->subcap = ipc_get_msg_cap(ipc_msg, 0);
		fd_dic[fd]->subcap_ipc_struct =
			ipc_register_client(fd_dic[fd]->subcap);
		int server_conn_id =
			((struct lwip_request *)ipc_get_msg_data(ipc_msg))->args[2];
		debug("CONN from client: get an cap: %d\n", fd_dic[fd]->subcap);
		// Setup myself as an IPC server and send my cap to TCPServer,
		//         so that TCPServer can send request to me.
		// HACK: I don't think we need a real full duplex connection. Most of
		//         the time, the communication pattern is client send to
		//         server and server replies. The server won't send to client
		//         proactively and the server won't leave client message
		//         unresponded. Thus, just don't build the reverse IPC
		//         channel until we encounter some problem.
		// The hack is no longer right.
		// New version: we use to ringbufferst to communicate; thus, we don't
		//         need to build bidirectional IPC channels.
		err = local_socket_connect(fd, server_conn_id);
		BUG_ON(err);
	}

	ipc_destroy_msg(ipc_msg);
	return err;
}

int chcore_sendto(int fd, const void *buf, size_t len, int flags,
		  const struct sockaddr *addr, socklen_t alen)
{
	int ret = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;
	int copylen = 0, remain = 0, bias = 0;

	debug("CHCORE SEND TO   \n");

	if (fd_dic[fd] == 0)
		return -EBADF;

	/* Check buf and addr cannot be NULL */
	if ((len != 0 && buf == 0) || (alen != 0 && addr == 0))
		return -EFAULT;

	/* If alen is larger than LWIP_DATA_LEN */
	BUG_ON(alen > LWIP_DATA_LEN);

	// Local Socket Fowarding
	if (fd_dic[fd]->subcap != NO_SUBCAP) {
		if (addr != NULL || alen != 0) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		if (flags) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		return local_socket_write(fd, buf, len);
	}

	ipc_msg =
	    ipc_create_msg(lwip_ipc_struct, sizeof(struct lwip_request), 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);

	lr_ptr->req = LWIP_SOCKET_SEND;
	lr_ptr->args[0] = fd_dic[fd]->conn_id;
	/* lr_ptr->args[1] (length of data) will be set later */
	lr_ptr->args[2] = flags;
	lr_ptr->args[3] = alen;

	if (len + alen > LWIP_DATA_LEN) {
		/* Init */
		copylen = 0;
		ret = 0;
		remain = len;
		/* If data is too large, seperate to multiple IPC */
		while (remain > 0 && ret == copylen) {
			/* If remain cannot send in one ipc, leave it to the
			 * next ipc */
			copylen = remain + alen > LWIP_DATA_LEN
				      ? LWIP_DATA_LEN - alen
				      : remain;
			lr_ptr->args[1] = copylen;
			memcpy(lr_ptr->data, (char *)buf + bias, copylen);
			memcpy(lr_ptr->data + copylen, (void *)addr, alen);
			if ((ret = ipc_call(lwip_ipc_struct, ipc_msg)) < 0)
				break; /* Error occur */
			bias += ret;
			remain -= ret;
		}
		ret = ret < 0 ? ret : bias;
	} else {
		/* Else one single ipc is enough */
		lr_ptr->args[1] = len;
		memcpy(lr_ptr->data, (void *)buf, len);
		memcpy(lr_ptr->data + len, (void *)addr, alen);
		ret = ipc_call(lwip_ipc_struct, ipc_msg);
	}
	ipc_destroy_msg(ipc_msg);
	return ret;
}

int chcore_recvfrom(int fd, void *restrict buf, size_t len, int flags,
		    struct sockaddr *restrict addr, socklen_t *restrict alen)
{
	int ret = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;
	int copylen = 0, remain = len, bias = 0;
	int alen_val = (alen != 0) ? *alen : 0;

	debug("CHCORE REVEI FROM\n");

	if (fd_dic[fd] == 0)
		return -EBADF;
	if ((len != 0 && buf == 0) || (alen != 0 && *alen != 0 && addr == 0))
		return -EFAULT;
	/* If alen is larger than LWIP_DATA_LEN */
	BUG_ON((alen != 0 && *alen > LWIP_DATA_LEN));

	// Local Socket Fowarding
	if (fd_dic[fd]->subcap != NO_SUBCAP) {
		if (addr != NULL || alen != NULL) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		if (flags) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		return local_socket_read(fd, buf, len);
	}

	ipc_msg =
	    ipc_create_msg(lwip_ipc_struct, sizeof(struct lwip_request), 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);

	lr_ptr->req = LWIP_SOCKET_RECV;
	lr_ptr->args[0] = fd_dic[fd]->conn_id;
	/* lr_ptr->args[1] (length of data) will be set later */
	lr_ptr->args[2] = flags;
	lr_ptr->args[3] = alen_val;

	/* If data is too large, seperate to multiple IPC */
	if (len + alen_val > LWIP_DATA_LEN) {
		ret = copylen = 0;
		remain = len;
		while (remain > 0 && ret == copylen) {
			copylen = remain + alen_val > LWIP_DATA_LEN
				      ? LWIP_DATA_LEN - alen_val
				      : remain;
			lr_ptr->args[1] = copylen;
			memcpy(lr_ptr->data + copylen, (void *)addr, alen_val);
			if ((ret = ipc_call(lwip_ipc_struct, ipc_msg)) < 0) {
				/* Already received messages? */
				ret = bias > 0 ? bias : ret;
				goto out;
			}
			BUG_ON(ret > LWIP_DATA_LEN);
			memcpy((char *)buf + bias, lr_ptr->data, ret);
			if (alen != 0 && *alen != 0) {
				*alen = lr_ptr->args[3];
				memcpy(addr, lr_ptr->data + copylen, *alen);
			}
			remain -= ret;
			bias += ret;
			// printf("%s: copylen=%d, bias=%d, ret=%d, remain=%d\n", __func__,
			//        copylen, bias, ret, remain);
			lr_ptr->args[2] |= MSG_DONTWAIT; /* Update the flag */
		}
		ret = ret < 0 ? ret : bias;
	} else {
		/* Else one single ipc is enough */
		lr_ptr->args[1] = len;
		if (alen != 0 && *alen != 0)
			memcpy(lr_ptr->data + len, (void *)addr, *alen);
		if ((ret = ipc_call(lwip_ipc_struct, ipc_msg)) < 0)
			goto out;
		BUG_ON(ret > LWIP_DATA_LEN);
		memcpy((void *)buf, lr_ptr->data, ret);
		if (alen != 0 && *alen != 0) {
			*alen = lr_ptr->args[3];
			memcpy((void *)addr, lr_ptr->data + len, *alen);
		}
		// printf("%s: copylen=%d, bias=%d, ret=%d\n", __func__,
		// 	       len, bias, ret);
	}
out:
	// printf("ret=%d\n", ret);
	ipc_destroy_msg(ipc_msg);
	return ret;
}

int chcore_sendmsg(int fd, const struct msghdr *msg, int flags)
{
	int ret = 0, len = 0, shared_pmo = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;

	debug("CHCORE SEND MSG \n");

	if (fd_dic[fd] == 0)
		return -EBADF;

	if ((len = get_msghdr_size((struct msghdr *)msg)) < 0)
		return len;

	// Local Socket Fowarding
	if (fd_dic[fd]->subcap != NO_SUBCAP) {
		if (msg->msg_name != NULL || msg->msg_namelen != 0) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		if (msg->msg_control != NULL || msg->msg_controllen != 0) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		if (msg->msg_flags) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		return local_socket_writev(fd, msg->msg_iov, msg->msg_iovlen);
	}

	/* Need transfer pmo if total len > LWIP_DATA_LEN */
	ipc_msg = ipc_create_msg(lwip_ipc_struct, sizeof(struct lwip_request),
				 len > LWIP_DATA_LEN ? 1 : 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	lr_ptr->req = LWIP_SOCKET_SMSG;
	lr_ptr->args[0] = fd_dic[fd]->conn_id;
	lr_ptr->args[1] = flags;
	lr_ptr->args[2] = len;
	if ((ret = pack_message((struct msghdr *)msg, lr_ptr->data, len, 1,
				&shared_pmo)) < 0)
		goto out;
	/* Transfer the pmo cap to the server */
	if (len > LWIP_DATA_LEN) {
		ipc_set_msg_cap(ipc_msg, 0, shared_pmo);
	}
	ret = ipc_call(lwip_ipc_struct, ipc_msg);
	/* TODO: Destroy PMO here */
out:
	ipc_destroy_msg(ipc_msg);
	return ret;
}

int chcore_recvmsg(int fd, struct msghdr *msg, int flags)
{
	int ret = 0, len = 0, shared_pmo = 0;
	ipc_msg_t *ipc_msg = 0;
	struct lwip_request *lr_ptr;
	char *recvbuf = NULL;

	debug("CHCORE RECV MSG \n");
	if (fd_dic[fd] == 0)
		return -EBADF;

	if ((len = get_msghdr_size((struct msghdr *)msg)) < 0)
		return len;

	// Local Socket Fowarding
	if (fd_dic[fd]->subcap != NO_SUBCAP) {
		if (msg->msg_name != NULL || msg->msg_namelen != 0) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		if (msg->msg_control != NULL || msg->msg_controllen != 0) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		if (msg->msg_flags) {
			printf("We don't support this!\n");
			BUG_ON(1);
		}
		return local_socket_readv(fd, msg->msg_iov, msg->msg_iovlen);
	}

	ipc_msg = ipc_create_msg(lwip_ipc_struct, sizeof(struct lwip_request),
				 len > LWIP_DATA_LEN ? 1 : 0);
	lr_ptr = (struct lwip_request *)ipc_get_msg_data(ipc_msg);
	lr_ptr->req = LWIP_SOCKET_RMSG;
	lr_ptr->args[0] = fd_dic[fd]->conn_id;
	lr_ptr->args[1] = flags;
	lr_ptr->args[2] = len;
	if ((ret = pack_message((struct msghdr *)msg, lr_ptr->data, len, 0,
				&shared_pmo)) < 0)
		goto out;
	if (len > LWIP_DATA_LEN) {
		ipc_set_msg_cap(ipc_msg, 0, shared_pmo);
		ret = ipc_call(lwip_ipc_struct, ipc_msg);
		recvbuf = malloc(len);
		usys_read_pmo(shared_pmo, 0, recvbuf, len);
		/* XXX: Directly read pmo in unpack message */
		unpack_message(msg, recvbuf);
		free(recvbuf);
		ipc_destroy_msg(ipc_msg);
	} else {
		ret = ipc_call(lwip_ipc_struct, ipc_msg);
		unpack_message(msg, lr_ptr->data);
	}
out:
	/* TODO: Destroy PMO here */
	ipc_destroy_msg(ipc_msg);
	return ret;
}

int chcore_shutdown(int fd, int how)
{
	if (fd_dic[fd] == 0)
		return -EBADF;
	return lwip_ipc(LWIP_SOCKET_STDW, NULL, 0, 2, fd_dic[fd]->conn_id, how);
}
