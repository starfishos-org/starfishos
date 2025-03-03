#pragma once

#include <sys/types.h>
#include <string.h>

#include "fd.h"

#define IS_HOSTFS(path) (strncmp(path, "/host/", 6) == 0)

#define HOSTFS_VADDR (0x100000000000)

extern struct fd_ops hostfs_ops;

struct hostfs_file_info {
    u64 start_vaddr;
    u64 size;
    u64 prot;
    u64 offset;
};

int chcore_hostfs_open(int fd, char *path);
int chcore_hostfs_pread(int fd, void *buf, size_t count, off_t offset);
int chcore_hostfs_pwrite(int fd, void *buf, size_t count, off_t offset);
