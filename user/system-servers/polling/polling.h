#pragma once

#include <limits.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <fs_wrapper_defs.h>

#define POLLING_SHM_SIZE (PAGE_SIZE * 10UL)

#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)

enum polling_shm_msg_state {
    MSG_FREE = 0,
    MSG_REQ_WRITING,
    MSG_REQ_READY,
    MSG_RESP_WRITING,
    MSG_RESP_READY,
};

enum polling_fs_request_type {
    POLLING_FS_REQ_OPEN,
    POLLING_FS_REQ_READ,
    POLLING_FS_REQ_WRITE,
    POLLING_FS_REQ_CLOSE,
    POLLING_FS_REQ_EMPTY,
};

struct polling_fs_req_open {
    char path[FS_REQ_PATH_BUF_LEN];
    int flags;
    int mode;
};

struct polling_fs_req_read {
    int fd;
    size_t count;
};

struct polling_fs_req_write {
    int fd;
    char buf[POLLING_FS_WRITE_BUF_SIZE];
    size_t count;
};

struct polling_fs_req_close {
    int fd;
};

struct polling_fs_req_empty {};

struct polling_fs_request {
    enum polling_fs_request_type type;
    union {
        struct polling_fs_req_open open;
        struct polling_fs_req_read read;
        struct polling_fs_req_write write;
        struct polling_fs_req_close close;
        struct polling_fs_req_empty empty;
    } __attribute__((aligned(8)));
};

struct polling_fs_resp_open {
    int fd;
};

struct polling_fs_resp_read {
    ssize_t count;
    char buf[POLLING_FS_READ_BUF_SIZE];
};

struct polling_fs_resp_write {
    ssize_t count;
};

struct polling_fs_resp_close {
    int ret;
};

struct polling_fs_resp_empty {};

struct polling_fs_response {
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
        struct polling_fs_resp_empty empty;
    } __attribute__((aligned(8)));
};

struct shm_msg {
    _Atomic int state;
    union {
        struct polling_fs_request fs_req;
        struct polling_fs_response fs_resp;
    } __attribute__((aligned(8)));
};

// calculate the maximum number of messages based on the shm size
#define MAX_MSG_COUNT (POLLING_SHM_SIZE / sizeof(struct shm_msg))

struct polling_shm_region {
    struct shm_msg msgs[MAX_MSG_COUNT];
    _Atomic int write_index; // next write position
    _Atomic int read_index; // next read position
};

static_assert(sizeof(struct polling_shm_region) <= POLLING_SHM_SIZE,
              "polling shm region size is too large");
