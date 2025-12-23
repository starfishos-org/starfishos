#pragma once

#include "polling.h"

void polling_enqueue_fs_request(struct shm_msg *msg,
                                struct polling_fs_request *req);
void polling_wait_for_response(struct shm_msg *msg);

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode);
ssize_t polling_fs_read(struct polling_shm_region *shm, int fd, void *buf,
                        size_t count);
ssize_t polling_fs_write(struct polling_shm_region *shm, int fd,
                         const void *buf, size_t count);
int polling_fs_close(struct polling_shm_region *shm, int fd);
void polling_fs_empty(struct polling_shm_region *shm);