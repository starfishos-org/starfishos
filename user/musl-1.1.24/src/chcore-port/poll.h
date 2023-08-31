#pragma once

#include <debug_lock.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>

struct epitem {
	int fd;
	struct epoll_event event;
	/* epitem list in the same eventpoll */
	struct list_head epi_node;
};

struct eventpoll {
	/* All epitems */
	int volatile epi_lock;
	struct list_head epi_list;
	uint32_t wait_count;
};

/* Use by poll wait */
struct pollarg {
	/* Notifc cap, used to notify the poller */
	int notifc_cap;
	/* Event mask */
	short int events;
};

int chcore_epoll_create1(int flags);
int chcore_epoll_ctl(int epfd, int op, int fd, struct epoll_event *events);
int chcore_epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
		       int timeout, const sigset_t *sigmask);

int chcore_poll(struct pollfd fds[], nfds_t nfds, int timeout);
int chcore_ppoll(struct pollfd *fds, nfds_t n, const struct timespec *tmo_p,
		 const sigset_t *sigmask);