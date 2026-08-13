#pragma once

#include <common/types.h>

/*
 * HostFS v2 is a synchronous request protocol over the hostfs ivshmem BAR.
 * The host owns the files; guest operations are forwarded to the host daemon,
 * so there is no second filesystem image to reconcile.
 */
#define HOSTFS_LIVE_MAGIC            "hostfs2"
#define HOSTFS_LIVE_VERSION          2U
#define HOSTFS_LIVE_HEADER_SIZE      4096U
#define HOSTFS_LIVE_SLOT_HEADER_SIZE 4096U
#define HOSTFS_LIVE_PATH_SIZE        512U
#define HOSTFS_LIVE_MAX_SLOTS        64U
#define HOSTFS_LIVE_MAX_DATA_SIZE    (4U * 1024U * 1024U)

/* HOSTFS_OP_PWRITE request flags. */
#define HOSTFS_IO_APPEND (1U << 0)

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

/*
 * This structure occupies the start of a 4 KiB slot header. The request data
 * starts HOSTFS_LIVE_SLOT_HEADER_SIZE bytes after the slot base.
 */
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
