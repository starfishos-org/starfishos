#pragma once

#include "polling.h"

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode);
ssize_t polling_fs_read(struct polling_shm_region *shm, int fd, void *buf,
                        size_t count);
ssize_t polling_fs_write(struct polling_shm_region *shm, int fd,
                         const void *buf, size_t count);
int polling_fs_close(struct polling_shm_region *shm, int fd);
void polling_fs_empty(struct polling_shm_region *shm);
void polling_kernel_flush_tlb(struct polling_shm_region *shm, u64 memcpy_src_pa,
                              u64 memcpy_dst_pa, u64 memcpy_len,
                              u64 memcpy_fault_va, u64 memcpy_vmspace);