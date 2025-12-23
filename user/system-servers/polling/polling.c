#include "polling.h"

#include "unistd.h"
#include <chcore/memory.h>
#include <chcore/syscall.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <stdio.h>
#include <stdatomic.h>

void init_polling_shm_region(struct polling_shm_region *shm)
{
    int my_id = usys_get_machine_id();
    atomic_init(&shm->write_index, 0);
    atomic_init(&shm->read_index, 0);
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        /* Set magic value to verify structure layout - each slot has unique magic */
        atomic_store((volatile u32 *)&shm->msgs[i].magic, SHM_MSG_MAGIC(i));
        /* Initialize message fields */
        atomic_store((volatile u32 *)&shm->msgs[i].type, SHM_MSG_TYPE_MAX);
        atomic_store((volatile u32 *)&shm->msgs[i].sender, 0xFFFFFFFF);
        atomic_store((volatile u64 *)&shm->msgs[i].lock, 0);
        set_msg_free((volatile enum shm_msg_flag *)&shm->msgs[i].flag);
        /* Initialize FS message fields */
        set_msg_free(&shm->msgs[i].msg.fs_req.flag);
        set_msg_free(&shm->msgs[i].msg.fs_reply.flag);
        /* Initialize TLB message fields */
        atomic_store((volatile u64 *)&shm->msgs[i].msg.tlb_req.memcpy_src_pa, 0);
        atomic_store((volatile u64 *)&shm->msgs[i].msg.tlb_req.memcpy_dst_pa, 0);
        atomic_store((volatile u64 *)&shm->msgs[i].msg.tlb_req.memcpy_len, 0);
        atomic_store((volatile u64 *)&shm->msgs[i].msg.tlb_req.memcpy_fault_va, 0);
        atomic_store((volatile u64 *)&shm->msgs[i].msg.tlb_req.memcpy_vmspace, 0);
        atomic_store((volatile u32 *)&shm->msgs[i].msg.tlb_req.reply_received, 0);
        atomic_store((volatile u32 *)&shm->msgs[i].msg.tlb_req.reply_from, 0xFFFFFFFF);
        atomic_store((volatile s32 *)&shm->msgs[i].msg.tlb_req.reply_result, 0);
    }
    printf("[POLLING] Machine %d: init_polling_shm_region completed\n", my_id);
}

/* Handle MSI memcpy_and_flush_tlb message */
int handle_msi_memcpy_and_flush_tlb_msg(struct msi_msg_slot *msg_slot, int sender_id)
{
    /* Read message parameters */
    u64 src_pa = atomic_load((volatile u64 *)&msg_slot->memcpy_src_pa);
    u64 dst_pa = atomic_load((volatile u64 *)&msg_slot->memcpy_dst_pa);
    u64 len = atomic_load((volatile u64 *)&msg_slot->memcpy_len);
    u64 fault_va = atomic_load((volatile u64 *)&msg_slot->memcpy_fault_va);
    u64 vmspace = atomic_load((volatile u64 *)&msg_slot->memcpy_vmspace);
    
    printf("[POLLING] Handling memcpy_and_flush_tlb: src_pa=0x%llx, dst_pa=0x%llx, len=%llu\n",
           src_pa, dst_pa, len);
    
    /* Note: User space cannot directly perform memcpy on physical addresses
     * or flush TLB. This needs to be handled by kernel through a syscall.
     * For now, we'll just acknowledge the message and let kernel handle it
     * through its own polling mechanism. */
    
    /* Mark message as processed and send reply */
    atomic_store((volatile u32 *)&msg_slot->reply_received, 1);
    atomic_store((volatile u32 *)&msg_slot->reply_from, usys_get_machine_id());
    
    /* Clear message fields */
    atomic_store((volatile u32 *)&msg_slot->msg_from, 0xFFFFFFFF);
    atomic_store((volatile u32 *)&msg_slot->msg_type, 0);
    atomic_store((volatile u64 *)&msg_slot->memcpy_src_pa, 0);
    atomic_store((volatile u64 *)&msg_slot->memcpy_dst_pa, 0);
    atomic_store((volatile u64 *)&msg_slot->memcpy_len, 0);
    atomic_store((volatile u64 *)&msg_slot->memcpy_fault_va, 0);
    atomic_store((volatile u64 *)&msg_slot->memcpy_vmspace, 0);
    
    return 0;
}

/* Unified message polling - checks all message sources and dispatches to appropriate handlers */
/* Returns 1 if a message was processed, 0 otherwise */
int poll_all_messages(struct polling_server_ctx *ctx)
{
    struct polling_shm_region *shm = ctx->shm;
    int my_id = ctx->my_machine_id;

    /* Check all message slots for readable messages */
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        struct shm_msg *msg = &shm->msgs[i];
        
        /* Verify magic value - each slot should have its unique magic number */
        u32 magic_val = atomic_load((volatile u32 *)&msg->magic);
        u32 expected_magic = SHM_MSG_MAGIC(i);
        if (magic_val != expected_magic) {
            /* Log magic mismatch for debugging */
            static int magic_mismatch_count = 0;
            magic_mismatch_count++;
            if (magic_mismatch_count % 1000000 == 0) {
                printf("[POLLING] Machine %d: Slot %d magic mismatch! Expected 0x%x, got 0x%x (shm=%p, msg=%p, count=%d)\n",
                       my_id, i, expected_magic, magic_val, shm, msg, magic_mismatch_count);
            }
            continue; /* Skip invalid slots */
        }
        
        /* Check message flag */
        u32 flag_val = atomic_load_explicit((volatile u32 *)&msg->flag, memory_order_acquire);
        u32 msg_type = atomic_load((volatile u32 *)&msg->type);
        u32 sender = atomic_load((volatile u32 *)&msg->sender);
        
        /* Debug: log all slot states periodically */
        static int check_count = 0;
        check_count++;
        if (check_count % 1000000 == 0 && check_count > 0) {
            printf("[POLLING] Machine %d checking slot %d: magic=0x%x, flag=%u, type=%u, sender=%u, shm=%p, msg=%p\n", 
                   my_id, i, magic_val, flag_val, msg_type, sender, shm, msg);
        }
        
        if (flag_val != SHM_MSG_READABLE) {
            continue; /* No message in this slot */
        }
        
        /* Found a readable message! */
        printf("[POLLING] Machine %d found readable message in slot %d: type=%u, sender=%u, magic=0x%x, shm=%p, msg=%p\n",
               my_id, i, msg_type, sender, magic_val, shm, msg);
        
        switch (msg_type) {
        case SHM_MSG_TYPE_FS_REQ: {
            handle_polling_fs_request(msg);
            return 1;
        }
        case SHM_MSG_TYPE_TLB_REQ: {  /* Same as MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB */
            printf("[POLLING] Machine %d found TLB message in slot %d: msg=%p, type=%u, sender=%u\n",
                   my_id, i, msg, msg_type, sender);
            
            /* Call syscall to handle memcpy_and_flush_tlb in kernel */
            u64 src_pa = atomic_load((volatile u64 *)&msg->msg.tlb_req.memcpy_src_pa);
            u64 dst_pa = atomic_load((volatile u64 *)&msg->msg.tlb_req.memcpy_dst_pa);
            u64 len = atomic_load((volatile u64 *)&msg->msg.tlb_req.memcpy_len);
            u64 fault_va = atomic_load((volatile u64 *)&msg->msg.tlb_req.memcpy_fault_va);
            u64 vmspace = atomic_load((volatile u64 *)&msg->msg.tlb_req.memcpy_vmspace);
            
            /* Call syscall to perform memcpy and flush TLB */
            int ret = usys_memcpy_and_flush_tlb(src_pa, dst_pa, len, fault_va, vmspace);
            
            /* Write reply back to the same slot where the request is */
            /* This way the sender knows which request is completed */
            /* Write reply fields first */
            atomic_store((volatile s32 *)&msg->msg.tlb_req.reply_result, ret);
            atomic_store((volatile u32 *)&msg->msg.tlb_req.reply_from, my_id);
            /* Use release semantics to ensure all writes are visible before setting reply_received */
            /* This acts as a memory barrier: all previous writes are visible after this */
            atomic_store_explicit((volatile u32 *)&msg->msg.tlb_req.reply_received, 1, memory_order_release);
            
            printf("[POLLING] Machine %d: Processed TLB request from machine %d in slot %d, result=%d\n", 
                   my_id, sender, i, ret);
            
            /* Clear the message flag so sender knows it can read the reply */
            /* Note: We keep the message data until sender clears it */
            
            return 1;
        }
        default:
            printf("[POLLING] Unknown message type: %u\n", msg_type);
            break;
        }
    }
    
    return 0; /* No message processed */
}

void *polling_reader_thread(void *arg)
{
    struct polling_server_ctx *ctx = arg;
    int processed = 0;

    while (1) {
        /* Unified polling: check all message sources and dispatch by type */
        processed = poll_all_messages(ctx);
        
        /* Small delay to avoid busy waiting if no message was processed */
        if (!processed) {
            __asm__ volatile("pause");
        }
    }

    free(ctx);

    return NULL;
}

void create_polling_thread(u32 shm_id, pthread_t *tid, void **shm_addr)
{
    int ret;
    struct polling_server_ctx *ctx; // free in the thread
    ctx = (struct polling_server_ctx *)malloc(sizeof(struct polling_server_ctx));
    if (ctx == NULL) {
        printf("Failed to allocate memory for ctx\n");
        return;
    }
    *shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    ret = usys_mmap_shm(shm_id, *shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", shm_id);
        return;
    }
    ctx->shm = (struct polling_shm_region *)(*shm_addr);
    ctx->my_machine_id = usys_get_machine_id();
    printf("[POLLING] Machine %d: create_polling_thread, shm_id=%u, shm=%p, shm_addr=%p\n",
           ctx->my_machine_id, shm_id, ctx->shm, *shm_addr);
    
    /* Verify magic numbers in shared memory */
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        u32 magic_val = atomic_load((volatile u32 *)&ctx->shm->msgs[i].magic);
        u32 expected_magic = SHM_MSG_MAGIC(i);
        u32 flag_val = atomic_load((volatile u32 *)&ctx->shm->msgs[i].flag);
        u32 type_val = atomic_load((volatile u32 *)&ctx->shm->msgs[i].type);
        u32 sender_val = atomic_load((volatile u32 *)&ctx->shm->msgs[i].sender);
        printf("[POLLING] Machine %d: Slot %d init check: magic=0x%x (expected=0x%x, match=%d), flag=%u, type=%u, sender=%u\n",
               ctx->my_machine_id, i, magic_val, expected_magic, (magic_val == expected_magic), flag_val, type_val, sender_val);
    }

    ret = pthread_create(tid, NULL, polling_reader_thread, ctx);
    if (ret != 0) {
        printf("Failed to create polling thread\n");
        return;
    }
}

void join_polling_thread(pthread_t tid, void *shm_addr)
{
    pthread_join(tid, NULL);
    shmdt(shm_addr);
}

void detach_polling_thread(pthread_t tid)
{
    pthread_detach(tid);
}

void polling_enqueue_fs_request(struct shm_msg *msg,
                                struct polling_fs_request *req)
{
    // wait for the message slot to be free
    wait_msg_free(&msg->msg.fs_req.flag);

    memcpy(&msg->msg.fs_req, req, sizeof(struct polling_fs_request));
    set_msg_readable(&msg->msg.fs_req.flag);
}

void polling_wait_for_response(struct shm_msg *msg)
{
    wait_msg_readable(&msg->msg.fs_reply.flag);
}

void handle_polling_fs_request(struct shm_msg *msg)
{
    switch (msg->msg.fs_req.type) {
    case POLLING_FS_REQ_OPEN:
        handle_polling_fs_open(msg);
        break;
    case POLLING_FS_REQ_READ:
        handle_polling_fs_read(msg);
        break;
    case POLLING_FS_REQ_WRITE:
        handle_polling_fs_write(msg);
        break;
    case POLLING_FS_REQ_CLOSE:
        handle_polling_fs_close(msg);
        break;
    default:
        printf("Unsupported polling fs request type: %d\n", msg->msg.fs_req.type);
        break;
    }
}

void handle_polling_fs_open(struct shm_msg *msg)
{
    char path[FS_REQ_PATH_BUF_LEN];
    int flags = msg->msg.fs_req.op.open.flags;
    int mode = msg->msg.fs_req.op.open.mode;
    strncpy(path, msg->msg.fs_req.op.open.path, strlen(msg->msg.fs_req.op.open.path));
    set_msg_free(&msg->msg.fs_req.flag);

    int fd = open(path, flags, mode);

    wait_msg_free(&msg->msg.fs_reply.flag);
    msg->msg.fs_reply.op.open.fd = fd;
    set_msg_readable(&msg->msg.fs_reply.flag);
}

void handle_polling_fs_read(struct shm_msg *msg)
{
    int fd = msg->msg.fs_req.op.read.fd;
    size_t count = msg->msg.fs_req.op.read.count;
    set_msg_free(&msg->msg.fs_req.flag);

    ssize_t ret = read(fd, msg->msg.fs_reply.op.read.buf, count);

    wait_msg_free(&msg->msg.fs_reply.flag);
    msg->msg.fs_reply.op.read.count = ret;
    set_msg_readable(&msg->msg.fs_reply.flag);
}

void handle_polling_fs_write(struct shm_msg *msg)
{
    int fd = msg->msg.fs_req.op.write.fd;
    size_t count = msg->msg.fs_req.op.write.count;
    void *buf = (void *)malloc(count);
    memcpy(buf, msg->msg.fs_req.op.write.buf, count);
    set_msg_free(&msg->msg.fs_req.flag);

    ssize_t ret = write(fd, buf, count);

    wait_msg_free(&msg->msg.fs_reply.flag);
    msg->msg.fs_reply.op.write.count = ret;
    set_msg_readable(&msg->msg.fs_reply.flag);
    free(buf);
}

void handle_polling_fs_close(struct shm_msg *msg)
{
    int fd = msg->msg.fs_req.op.close.fd;
    set_msg_free(&msg->msg.fs_req.flag);

    int ret = close(fd);

    wait_msg_free(&msg->msg.fs_reply.flag);
    msg->msg.fs_reply.op.close.ret = ret;
    set_msg_readable(&msg->msg.fs_reply.flag);
}

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode)
{
    struct polling_fs_request req = {
            .type = POLLING_FS_REQ_OPEN,
            .op.open =
                    {
                            .flags = flags,
                            .mode = mode,
                    },
    };
    strncpy(req.op.open.path, path, FS_REQ_PATH_BUF_LEN);
    int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
    struct shm_msg *msg = &shm->msgs[idx];
    polling_enqueue_fs_request(msg, &req);

    wait_msg_readable(&msg->msg.fs_reply.flag);
    int fd = msg->msg.fs_reply.op.open.fd;
    set_msg_free(&msg->msg.fs_reply.flag);

    return fd;
}

ssize_t polling_fs_read(struct polling_shm_region *shm, int fd, void *buf,
                        size_t count)
{
    size_t left_count = count;
    void *buf_ptr = (void *)buf;
    ssize_t total_count = 0;
    while (left_count > 0) {
        size_t read_count = MIN(left_count, POLLING_FS_READ_BUF_SIZE);
        struct polling_fs_request req = {
                .type = POLLING_FS_REQ_READ,
                .op.read =
                        {
                                .fd = fd,
                                .count = read_count,
                        },
        };
        int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
        struct shm_msg *msg = &shm->msgs[idx];
        polling_enqueue_fs_request(msg, &req);

        wait_msg_readable(&msg->msg.fs_reply.flag);
        ssize_t response_read_count = msg->msg.fs_reply.op.read.count;
        memcpy(buf_ptr, msg->msg.fs_reply.op.read.buf, response_read_count);
        set_msg_free(&msg->msg.fs_reply.flag);

        buf_ptr += response_read_count;
        left_count -= response_read_count;
        total_count += response_read_count;
        if (response_read_count < read_count) {
            break;
        }
    }
    return total_count;
}

ssize_t polling_fs_write(struct polling_shm_region *shm, int fd,
                         const void *buf, size_t count)
{
    size_t left_count = count;
    void *buf_ptr = (void *)buf;
    ssize_t total_count = 0;
    while (left_count > 0) {
        size_t write_count = MIN(left_count, POLLING_FS_WRITE_BUF_SIZE);
        struct polling_fs_request req = {
                .type = POLLING_FS_REQ_WRITE,
                .op.write =
                        {
                                .fd = fd,
                                .count = write_count,
                        },
        };
        memcpy(req.op.write.buf, buf_ptr, write_count);
        int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
        struct shm_msg *msg = &shm->msgs[idx];
        polling_enqueue_fs_request(msg, &req);

        wait_msg_readable(&msg->msg.fs_reply.flag);
        ssize_t response_write_count = msg->msg.fs_reply.op.write.count;
        set_msg_free(&msg->msg.fs_reply.flag);

        buf_ptr += response_write_count;
        left_count -= response_write_count;
        total_count += response_write_count;
        if (response_write_count < write_count) {
            break;
        }
    }
    return total_count;
}

int polling_fs_close(struct polling_shm_region *shm, int fd)
{
    struct polling_fs_request req = {
            .type = POLLING_FS_REQ_CLOSE,
            .op.close =
                    {
                            .fd = fd,
                    },
    };
    int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
    struct shm_msg *msg = &shm->msgs[idx];
    polling_enqueue_fs_request(msg, &req);

    wait_msg_readable(&msg->msg.fs_reply.flag);
    int ret = msg->msg.fs_reply.op.close.ret;
    set_msg_free(&msg->msg.fs_reply.flag);

    return ret;
}