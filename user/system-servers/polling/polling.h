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
#include <stdint.h>
#include <fs_wrapper_defs.h>

/* Type definitions for compatibility */
#ifndef u32
typedef uint32_t u32;
#endif
#ifndef u64
typedef uint64_t u64;
#endif

#define POLLING_SHM_SIZE  (PAGE_SIZE * 10UL)
#define MAX_MSG_COUNT     4
#define POLLING_FS_SHM_ID 0

#define POLLING_FS_WRITE_BUF_SIZE (PAGE_SIZE)
#define POLLING_FS_READ_BUF_SIZE  (PAGE_SIZE)

enum shm_msg_flag {
    SHM_MSG_FREE = 0,
    SHM_MSG_READABLE = 1,
};

/* Magic value to verify shm_msg structure layout */
/* Each message slot has a unique magic number: SHM_MSG_MAGIC_BASE + slot_index */
#define SHM_MSG_MAGIC_BASE 0xDEAD0000
#define SHM_MSG_MAGIC(slot_idx) (SHM_MSG_MAGIC_BASE + (slot_idx))
#define SHM_MSG_MAGIC_INVALID 0x00000000

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

/* Message types - directly use MSI message types for TLB */
enum shm_msg_type {
    SHM_MSG_TYPE_FS_REQ = 0,                      /* File system request */
    SHM_MSG_TYPE_FS_REPLY = 1,                    /* File system reply */
    SHM_MSG_TYPE_TLB_REQ = 2,                     /* TLB request (same as MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB) */
    SHM_MSG_TYPE_MAX
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
    volatile u64 lock;             /* Lock for synchronization (simple spinlock: 0=unlocked, 1=locked) */
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

static_assert(sizeof(struct polling_shm_region) <= POLLING_SHM_SIZE,
              "polling_shm_region size is too large");

/* MSI message types (matching kernel definitions) */
enum msi_msg_type {
    MSI_MSG_TYPE_TLB_FLUSH = 0,
    MSI_MSG_TYPE_TEST = 1,
    MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB = 2,
    MSI_MSG_TYPE_MAX
};

/* MSI message slot structure (matching kernel dsm_metadata_t.msi_test_msg) */
struct msi_msg_slot {
    volatile u32 msg_from;      /* Source machine ID */
    volatile u32 msg_type;      /* Message type */
    volatile u32 reply_received; /* Reply received flag */
    volatile u32 reply_from;    /* Reply from machine ID */
    /* Message-specific data */
    volatile u64 tlb_start_va;
    volatile u64 tlb_len;
    volatile u64 tlb_vmspace;
    /* For MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB: */
    volatile u64 memcpy_src_pa;
    volatile u64 memcpy_dst_pa;
    volatile u64 memcpy_len;
    volatile u64 memcpy_fault_va;
    volatile u64 memcpy_vmspace;
    /* Lock (we'll use atomic operations instead) */
    volatile u64 lock_padding[8]; /* Padding for lock structure */
};

struct polling_server_ctx {
    struct polling_shm_region *shm;      /* Pointer to this machine's polling shm region */
    struct msi_msg_slot *msi_msg_slots;  /* Pointer to dsm_meta->msi_test_msg array (for MSI mode) */
    int my_machine_id;                    /* Current machine ID */
    int cluster_machine_num;              /* Number of machines in cluster */
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
void create_polling_thread_with_dsm(struct msi_msg_slot *msi_msg_slots, 
                                    int my_machine_id, int cluster_machine_num,
                                    u32 shm_id, pthread_t *tid, void **shm_addr);
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

/* Handle MSI messages from dsm_meta */
int handle_msi_memcpy_and_flush_tlb_msg(struct msi_msg_slot *msg_slot, int sender_id);
/* Unified message polling - checks all message sources and dispatches to appropriate handlers */
/* Returns 1 if a message was processed, 0 otherwise */
int poll_all_messages(struct polling_server_ctx *ctx);