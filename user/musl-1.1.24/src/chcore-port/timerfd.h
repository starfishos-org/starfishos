#pragma once

#include <sys/timerfd.h>
#include <poll.h>

int chcore_timerfd_create(int clockid, int flags);
int chcore_timerfd_settime(int fd, int flags,
			   struct itimerspec *new_value,
			   struct itimerspec *old_value);
int chcore_timerfd_gettime(int fd, struct itimerspec *curr_value);
int chcore_timerfd_async_poll(struct pollfd fds[], nfds_t nfds, int timeout, int notifc_cap);
