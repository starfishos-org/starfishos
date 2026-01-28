#pragma once

#include "polling_config.h"

#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <fs_wrapper_defs.h>

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
    POLLING_PRINT_DEBUG_INFO,
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

/* Structure for batch memcpy and flush TLB operations */
/* Must match kernel/syscall/syscall.c:struct memcpy_flush_tlb_op */
struct memcpy_flush_tlb_op {
    u64 src_pa;
    u64 dst_pa;
    u64 len;
    u64 fault_va;
    u64 vmspace_ptr;
};

struct polling_req_print_debug_info {};

struct polling_request {
    enum polling_request_type type;
    union {
        struct polling_fs_req_open open;
        struct polling_fs_req_read read;
        struct polling_fs_req_write write;
        struct polling_fs_req_close close;
        struct polling_req_empty empty;
        struct polling_kernel_req_flush_tlb flush_tlb;
        struct polling_req_print_debug_info print_debug_info;
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

struct polling_resp_print_debug_info {};

struct polling_response {
    union {
        struct polling_fs_resp_open open;
        struct polling_fs_resp_read read;
        struct polling_fs_resp_write write;
        struct polling_fs_resp_close close;
        struct polling_resp_empty empty;
        struct polling_kernel_resp_flush_tlb flush_tlb;
        struct polling_resp_print_debug_info print_debug_info;
    } __attribute__((aligned(8)));
};

#if REUSE_REQ_RESP_BUFFER == true
struct shm_msg {
    _Atomic int state;
    union {
        struct polling_request req;
        struct polling_response resp;
    } __attribute__((aligned(64)));
};

#else

struct shm_msg {
    _Atomic int state;
    struct polling_request req;
    struct polling_response resp;
};

#endif

// calculate the maximum number of messages based on the shm size
#define MAX_MSG_COUNT (POLLING_SHM_SIZE / sizeof(struct shm_msg))

struct polling_shm_region {
    struct shm_msg msgs[MAX_MSG_COUNT];
    _Atomic int write_index; // next write position
    _Atomic int read_index; // next read position
};

static_assert(sizeof(struct polling_shm_region) <= POLLING_SHM_SIZE,
              "polling shm region size is too large");
