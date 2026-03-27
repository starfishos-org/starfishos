#pragma once

#include "polling.h"

/* ---- Allocator ---- */
struct dq_node *dq_alloc_node(struct polling_shm_region *shm);
void dq_free_node(struct polling_shm_region *shm, struct dq_node *node);

/* ---- Queue operations ---- */
void dq_enqueue(struct polling_shm_region *shm, struct dq_node *node,
                struct polling_request *req);
void dq_wait_for_done(struct dq_node *node);

/* ---- High-level FS operations (producer side) ---- */
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
void polling_print_debug_info(struct polling_shm_region *shm);

/* Debug */
void debug_print_shm_region(struct polling_shm_region *shm);
void debug_print_mpsc_alloc_msg_retry_time(void);
