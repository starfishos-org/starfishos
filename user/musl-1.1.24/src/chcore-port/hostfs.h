#pragma once

#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

#include "fd.h"

#define HOSTFS_MOUNT                 "/host"
#define HOSTFS_LIVE_MAGIC            "hostfs2"
#define HOSTFS_LIVE_VERSION          2U
#define HOSTFS_LIVE_HEADER_SIZE      4096U
#define HOSTFS_LIVE_SLOT_HEADER_SIZE 4096U
#define HOSTFS_LIVE_PATH_SIZE        512U
#define HOSTFS_IO_APPEND             (1U << 0)

static inline bool is_hostfs_path(const char *path)
{
    return path && strncmp(path, HOSTFS_MOUNT, sizeof(HOSTFS_MOUNT) - 1) == 0
           && (path[sizeof(HOSTFS_MOUNT) - 1] == '\0'
               || path[sizeof(HOSTFS_MOUNT) - 1] == '/');
}

#define IS_HOSTFS(path) is_hostfs_path(path)

enum hostfs_live_slot_state {
    HOSTFS_SLOT_FREE = 0,
    HOSTFS_SLOT_CLAIMED = 1,
    HOSTFS_SLOT_READY = 2,
    HOSTFS_SLOT_DONE = 3,
};

enum hostfs_live_opcode {
    HOSTFS_OP_OPEN = 1,
    HOSTFS_OP_CLOSE,
    HOSTFS_OP_PREAD,
    HOSTFS_OP_PWRITE,
    HOSTFS_OP_STAT,
    HOSTFS_OP_FSTAT,
    HOSTFS_OP_FSYNC,
    HOSTFS_OP_FTRUNCATE,
    HOSTFS_OP_GETDENTS,
    HOSTFS_OP_MKDIR,
    HOSTFS_OP_UNLINK,
    HOSTFS_OP_RMDIR,
    HOSTFS_OP_RENAME,
    HOSTFS_OP_ACCESS,
    HOSTFS_OP_READLINK,
    HOSTFS_OP_SYMLINK,
    HOSTFS_OP_FCNTL,
    HOSTFS_OP_FALLOCATE,
    HOSTFS_OP_STATFS,
};

struct hostfs_live_header {
    char magic[8];
    u32 version;
    u32 slot_count;
    u32 slot_stride;
    u32 data_size;
    u64 server_epoch;
    volatile u64 heartbeat;
};

struct hostfs_live_slot {
    volatile u32 state;
    u32 sequence;
    u32 opcode;
    u32 flags;
    s64 result;
    u64 handle;
    u64 offset;
    u64 count;
    u64 size;
    u64 mode;
    u64 ino;
    u64 atime_ns;
    u64 mtime_ns;
    u64 ctime_ns;
    u32 path_length;
    u32 uid;
    u32 gid;
    u32 nlink;
    u64 blocks;
    u64 block_size;
    char path[HOSTFS_LIVE_PATH_SIZE];
};

struct pci_hostfs_connect_info {
    u64 pmo_cap;
    u64 map_size;
    u32 slot_count;
    u32 slot_stride;
    u32 data_size;
    u32 version;
};

struct hostfs_metadata {
    u64 size;
    u64 mode;
    u64 ino;
    u64 atime_ns;
    u64 mtime_ns;
    u64 ctime_ns;
    u32 uid;
    u32 gid;
    u32 nlink;
    u64 blocks;
    u64 block_size;
};

struct hostfs_file_info {
    u64 handle;
    u64 file_size;
    u64 fd_offset;
    u64 mode;
    int flags;
    volatile u32 refcount;
    pthread_mutex_t lock;
    char file_name[HOSTFS_LIVE_PATH_SIZE];
};

extern struct fd_ops hostfs_ops;

int chcore_hostfs_open(int fd, const char *path, int flags, mode_t mode);
int chcore_hostfs_stat(int fd, const char *path, int flags,
                       struct stat *statbuf, size_t bufsize);
int chcore_hostfs_statfs(struct statfs *statbuf);
int chcore_hostfs_fsync(int fd);
int chcore_hostfs_ftruncate(int fd, off_t length);
int chcore_hostfs_fallocate(int fd, int mode, off_t offset, off_t length);
int chcore_hostfs_getdents64(int fd, char *buf, int count);
int chcore_hostfs_mkdir(const char *path, mode_t mode);
int chcore_hostfs_unlink(const char *path, int flags);
int chcore_hostfs_rename(const char *old_path, const char *new_path);
int chcore_hostfs_access(const char *path, int mode, int flags);
int chcore_hostfs_readlink(const char *path, char *buf, size_t bufsiz);
int chcore_hostfs_symlink(const char *target, const char *link_path);
const char *chcore_hostfs_path(int fd);
