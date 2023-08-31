#pragma once

#include <poll.h>
#include <sys/socket.h>

/* Batched poll socket fds */
int chcore_socket_server_poll(struct pollfd fds[], nfds_t nfds, int timeout,
			      int notifc_cap);

/* Socket basic operations */
int chcore_socket(int domain, int type, int protocol);
int chcore_setsocketopt(int fd, int level, int optname, const void *optval,
			socklen_t optlen);
int chcore_getsocketopt(int fd, int level, int optname, void *restrict optval,
			socklen_t *restrict optlen);
int chcore_getsockname(int fd, struct sockaddr *restrict addr,
		       socklen_t *restrict len);
int chcore_getpeername(int fd, struct sockaddr *restrict addr,
		       socklen_t *restrict len);
int chcore_bind(int fd, const struct sockaddr *addr, socklen_t len);
int chcore_listen(int fd, int backlog);
int chcore_accept(int fd, struct sockaddr *restrict addr,
		  socklen_t *restrict len);
int chcore_connect(int fd, const struct sockaddr *addr, socklen_t len);
int chcore_sendto(int fd, const void *buf, size_t len, int flags,
		  const struct sockaddr *addr, socklen_t alen);
int chcore_sendmsg(int fd, const struct msghdr *msg, int flags);
int chcore_recvmsg(int fd, struct msghdr *msg, int flags);
int chcore_recvfrom(int fd, void *restrict buf, size_t len, int flags,
		    struct sockaddr *restrict addr, socklen_t *restrict alen);
int chcore_shutdown(int fd, int how);

int local_socket_check_fd(int fd);
int local_socket_poll(struct pollfd fds[], nfds_t nfds, int timeout,
                      bool update_fds);
int local_socket_epoll(struct eventpoll *ep, int timeout,
                       struct epoll_event *events);
