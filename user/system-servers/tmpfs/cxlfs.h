#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct inode;

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define CXLFS_SHM_SIZE      (1UL << 30)
#ifndef CLUSTER_MAX_MACHINE_NUM
#define CLUSTER_MAX_MACHINE_NUM 8
#endif
#define CXLFS_SHM_ID_BASE   (2 * CLUSTER_MAX_MACHINE_NUM)
#define CXLFS_SHM_ID(mid)   (CXLFS_SHM_ID_BASE + (mid))

/* Mount an existing CXL disk image or format a new one. Returns 1 if fresh. */
int cxlfs_mount(int fs_machine, struct inode *runtime_root);
int cxlfs_restore_tree(struct inode *runtime_root);
int cxlfs_is_mounted(void);
int cxlfs_is_restoring(void);

int cxlfs_create_node(uint64_t parent_ino, const char *name, size_t len,
		      uint32_t type, mode_t mode, uint64_t *ino_out);
int cxlfs_unlink_node(uint64_t parent_ino, const char *name, size_t len);
int cxlfs_rename_node(uint64_t old_parent, const char *old_name,
		      size_t old_len, uint64_t new_parent,
		      const char *new_name, size_t new_len);

ssize_t cxlfs_read(uint64_t ino, off_t offset, void *buf, size_t size);
ssize_t cxlfs_write(uint64_t ino, off_t offset, const void *buf, size_t size);
int cxlfs_truncate(uint64_t ino, size_t size);
int cxlfs_allocate(uint64_t ino, off_t offset, off_t len, int keep_size);
void *cxlfs_page_addr(uint64_t ino, uint64_t page_no);

/* Flush metadata and publish a new stable superblock generation. */
int cxlfs_sync(void);
uint64_t cxlfs_generation(void);
int cxlfs_machine(void);
uint64_t cxlfs_root_ino(void);
