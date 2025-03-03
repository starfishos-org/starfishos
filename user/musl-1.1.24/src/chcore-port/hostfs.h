#pragma once

#include <sys/types.h>
#include <string.h>

#include "fd.h"

#define HOSTFS_PREFIX "/host/"
#define IS_HOSTFS(path) (strncmp(path, HOSTFS_PREFIX, strlen(HOSTFS_PREFIX)) == 0)

extern struct fd_ops hostfs_ops;

struct hostfs_file_info {
    u64 file_size;
    char file_name[128];
    u64 mmap_vaddr;
    u64 mmap_size;
    u64 mmap_prot;
    // the following fields are not used by kernel
    u64 fd_offset;
    u64 is_mapped;
};

int chcore_hostfs_open(int fd, char *path);
