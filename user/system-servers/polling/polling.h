#pragma once

#include "limits.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <fs_wrapper_defs.h>

#define POLLING_SHM_SIZE  (PAGE_SIZE * 10UL)
#define MAX_MSG_COUNT     4
#define POLLING_FS_SHM_ID 0

#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)

enum shm_msg_flag {
    SHM_MSG_FREE = 0,
    SHM_MSG_READABLE = 1,
};

enum polling_fs_request_type {
    POLLING_FS_REQ_OPEN,
    POLLING_FS_REQ_READ,
    POLLING_FS_REQ_WRITE,
    POLLING_FS_REQ_CLOSE,
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

struct polling_fs_request {
    enum shm_msg_flag flag;
    enum polling_fs_request_type type;
    union {
        struct polling_fs_req_open open;
        struct polling_fs_req_read read;
        struct polling_fs_req_write write;
        struct polling_fs_req_close close;
    } __attribute__((aligned(8))) op;
};

struct polling_fs_resp_open {
    int fd;
};

struct polling_fs_resp_read {
    ssize_t count;
    char buf[POLLING_FS_WRITE_BUF_SIZE];
};

struct polling_fs_resp_write {
    ssize_t count;
};

struct polling_fs_resp_close {
    int ret;
};

struct polling_fs_response {
    enum shm_msg_flag flag;
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
    } __attribute__((aligned(8))) op;
};

struct shm_msg {
    enum shm_msg_flag flag;
    struct polling_fs_request fs_req;
    struct polling_fs_response fs_resp;
};

struct polling_shm_region {
    struct shm_msg msgs[MAX_MSG_COUNT];
    int write_index; // next write position
    int read_index; // next read position
};

static_assert(sizeof(struct polling_shm_region) <= POLLING_SHM_SIZE,
              "polling shm region size is too large");

struct polling_server_ctx {
    struct polling_shm_region *shm;
};

inline void set_msg_readable(enum shm_msg_flag *flag)
{
    atomic_store(flag, SHM_MSG_READABLE);
}

inline void set_msg_free(enum shm_msg_flag *flag)
{
    atomic_store(flag, SHM_MSG_FREE);
}

inline void wait_msg_readable(enum shm_msg_flag *flag)
{
    while (atomic_load(flag) != SHM_MSG_READABLE) {
        // busy polling
    }
}

inline void wait_msg_free(enum shm_msg_flag *flag)
{
    while (atomic_load(flag) != SHM_MSG_FREE) {
        // busy polling
    }
}

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode);

ssize_t polling_fs_read(struct polling_shm_region *shm, int fd, void *buf,
                        size_t count);

ssize_t polling_fs_write(struct polling_shm_region *shm, int fd,
                         const void *buf, size_t count);

int polling_fs_close(struct polling_shm_region *shm, int fd);

void init_polling_shm_region(struct polling_shm_region *shm);

void create_polling_thread(u32 shm_id, pthread_t *tid, void **shm_addr);
void join_polling_thread(pthread_t tid, void *shm_addr);
void detach_polling_thread(pthread_t tid);

void handle_polling_fs_request(struct shm_msg *msg);

void handle_polling_fs_open(struct shm_msg *msg);
void handle_polling_fs_read(struct shm_msg *msg);
void handle_polling_fs_write(struct shm_msg *msg);
void handle_polling_fs_close(struct shm_msg *msg);

void polling_enqueue_fs_request(struct shm_msg *msg,
                                struct polling_fs_request *req);
void polling_wait_for_response(struct shm_msg *msg);