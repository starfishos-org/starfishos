#pragma once

#include <common/types.h>
#include <common/lock.h>
#include <dsm/dsm-single.h>
#include <mm/mm.h>
#include <posix/sys/types.h>

#define REUSE_REQ_RESP_BUFFER false
#define POLLING_SHM_SIZE (PAGE_SIZE * 10UL)
#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)
#define FS_REQ_PATH_BUF_LEN 256

enum polling_shm_msg_state {
    MSG_FREE = 0,
    MSG_REQ_WRITING,
    MSG_REQ_READY,
    MSG_RESP_WRITING,
    MSG_RESP_READY,
};

enum polling_request_type {
    POLLING_FS_REQ_OPEN,
    POLLING_FS_REQ_READ,
    POLLING_FS_REQ_WRITE,
    POLLING_FS_REQ_CLOSE,
    POLLING_REQ_EMPTY,
    POLLING_KERNEL_REQ_FLUSH_TLB,
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

struct polling_req_empty {};

struct polling_kernel_req_flush_tlb {
    u64 memcpy_src_pa;
    u64 memcpy_dst_pa;
    u64 memcpy_len;
    u64 memcpy_fault_va;
    u64 memcpy_vmspace;
};

struct polling_request {
    enum polling_request_type type;
    union {
        struct polling_fs_req_open open;
        struct polling_fs_req_read read;
        struct polling_fs_req_write write;
        struct polling_fs_req_close close;
        struct polling_req_empty empty;
        struct polling_kernel_req_flush_tlb flush_tlb;
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

struct polling_resp_empty {};

struct polling_kernel_resp_flush_tlb {
    u32 reply_received; /* Reply received flag: 0=not received, 1=received */
    u32 reply_from; /* Reply from machine ID */
    s32 reply_result; /* Reply result: 0=success, negative=error */
};

struct polling_response {
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
        struct polling_resp_empty empty;
        struct polling_kernel_resp_flush_tlb flush_tlb;
    } __attribute__((aligned(8)));
};

#if REUSE_REQ_RESP_BUFFER == true
struct shm_msg {
    int state;
    union {
        struct polling_request req;
        struct polling_response resp;
    } __attribute__((aligned(8)));
};

#else

struct shm_msg {
    int state;
    struct polling_request req;
    struct polling_response resp;
};

#endif

// calculate the maximum number of messages based on the shm size
#define MAX_MSG_COUNT (POLLING_SHM_SIZE / sizeof(struct shm_msg))

struct polling_shm_region {
    struct shm_msg msgs[MAX_MSG_COUNT];
    int write_index; // next write position
    int read_index; // next read position
};

void shm_init(void);
int sys_mmap_shm(u32 shm_id, void *addr);
struct shm_msg *mpsc_alloc_msg_retry(struct polling_shm_region *shm);
struct shm_msg *mpsc_alloc_msg(struct polling_shm_region *shm);
void polling_publish_request(struct shm_msg *msg, struct polling_request *req);
void polling_wait_for_response(struct shm_msg *msg);
void polling_free_msg(struct shm_msg *msg);