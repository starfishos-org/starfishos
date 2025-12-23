#pragma once

#include <common/types.h>
#include <common/lock.h>
#include <dsm/dsm-single.h>
#include <mm/mm.h>
#include <posix/sys/types.h>

#define SHM_DATA_SIZE (PAGE_SIZE * 10UL)

/* Shared memory definitions (shared between kernel and user-space) */
/* Each machine has its own shared memory region: dsm_meta->shm_data[machine_id] */
#define MAX_MSG_COUNT 4

enum shm_msg_flag {
    SHM_MSG_FREE = 0,
    SHM_MSG_READABLE = 1,
};

/* Magic value to verify shm_msg structure layout */
/* Each message slot has a unique magic number: SHM_MSG_MAGIC_BASE + slot_index */
#define SHM_MSG_MAGIC_BASE 0xDEAD0000
#define SHM_MSG_MAGIC(slot_idx) (SHM_MSG_MAGIC_BASE + (slot_idx))
#define SHM_MSG_MAGIC_INVALID 0x00000000

/* Message types - directly use MSI message types for TLB */
enum shm_msg_type {
    SHM_MSG_TYPE_FS_REQ = 0,                      /* File system request */
    SHM_MSG_TYPE_FS_REPLY = 1,                    /* File system reply */
    SHM_MSG_TYPE_TLB_REQ = 2,                     /* TLB request (same as MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB) */
    SHM_MSG_TYPE_MAX
};

/* FS request/response structures - must match userspace definitions */
#define FS_REQ_PATH_BUF_LEN 256
#define POLLING_FS_WRITE_BUF_SIZE PAGE_SIZE
#define POLLING_FS_READ_BUF_SIZE PAGE_SIZE

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

/* TLB request/reply structure - request and reply in the same slot */
struct shm_tlb_req {
    /* Request fields */
    volatile u64 memcpy_src_pa;
    volatile u64 memcpy_dst_pa;
    volatile u64 memcpy_len;
    volatile u64 memcpy_fault_va;
    volatile u64 memcpy_vmspace;
    /* Reply fields - in the same slot */
    volatile u32 reply_received;   /* Reply received flag: 0=not received, 1=received */
    volatile u32 reply_from;       /* Reply from machine ID */
    volatile s32 reply_result;     /* Reply result: 0=success, negative=error */
};

/* Unified message structure */
struct shm_msg {
    u32 magic;                     /* Magic value to verify structure layout (must be first field) */
    volatile u32 type;             /* Message type: SHM_MSG_TYPE_* */
    volatile u32 sender;           /* Sender machine ID */
    struct lock lock;               /* Lock for synchronization */
    enum shm_msg_flag flag;       /* Message flag: SHM_MSG_FREE or SHM_MSG_READABLE */
    union {
        struct polling_fs_request fs_req;
        struct polling_fs_response fs_reply;
        struct shm_tlb_req tlb_req;
    } __attribute__((aligned(8))) msg;
};

/* Polling shared memory region - each machine has one */
struct polling_shm_region {
    struct shm_msg msgs[MAX_MSG_COUNT];
    int write_index; // next write position
    int read_index; // next read position
};

void shm_init(void);
int sys_mmap_shm(u32 shm_id, void *addr);