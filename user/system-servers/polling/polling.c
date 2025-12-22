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
    atomic_init(&shm->write_index, 0);
    atomic_init(&shm->read_index, 0);
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        set_msg_free(&shm->msgs[i].fs_req.flag);
        set_msg_free(&shm->msgs[i].fs_resp.flag);
        set_msg_free(&shm->msgs[i].flag);
    }
}

void *polling_reader_thread(void *arg)
{
    struct polling_server_ctx *ctx = arg;
    struct polling_shm_region *shm = ctx->shm;
    struct shm_msg *msg;
    int idx;

    while (1) {
        idx = atomic_load(&shm->read_index) % MAX_MSG_COUNT;
        msg = &shm->msgs[idx];

        if (atomic_load_explicit(&msg->fs_req.flag, memory_order_acquire)
            == SHM_MSG_READABLE) {
            handle_polling_fs_request(msg);
            atomic_fetch_add(&shm->read_index, 1);
        } else {
            // no message, busy polling
        }
    }

    free(ctx);

    return NULL;
}

void create_polling_thread(u32 shm_id, pthread_t *tid, void **shm_addr)
{
    int ret;
    struct polling_server_ctx *ctx; // free in the thread
    ctx = (struct polling_server_ctx *)malloc(
            sizeof(struct polling_server_ctx));
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
    init_polling_shm_region(ctx->shm);
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
    wait_msg_free(&msg->fs_req.flag);

    memcpy(&msg->fs_req, req, sizeof(struct polling_fs_request));
    set_msg_readable(&msg->fs_req.flag);
}

void polling_wait_for_response(struct shm_msg *msg)
{
    wait_msg_readable(&msg->fs_resp.flag);
}

void handle_polling_fs_request(struct shm_msg *msg)
{
    switch (msg->fs_req.type) {
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
        printf("Unsupported polling fs request type: %d\n", msg->fs_req.type);
        break;
    }
}

void handle_polling_fs_open(struct shm_msg *msg)
{
    char path[FS_REQ_PATH_BUF_LEN];
    int flags = msg->fs_req.op.open.flags;
    int mode = msg->fs_req.op.open.mode;
    strncpy(path, msg->fs_req.op.open.path, strlen(msg->fs_req.op.open.path));
    set_msg_free(&msg->fs_req.flag);

    int fd = open(path, flags, mode);

    wait_msg_free(&msg->fs_resp.flag);
    msg->fs_resp.op.open.fd = fd;
    set_msg_readable(&msg->fs_resp.flag);
}

void handle_polling_fs_read(struct shm_msg *msg)
{
    int fd = msg->fs_req.op.read.fd;
    size_t count = msg->fs_req.op.read.count;
    set_msg_free(&msg->fs_req.flag);

    ssize_t ret = read(fd, msg->fs_resp.op.read.buf, count);

    wait_msg_free(&msg->fs_resp.flag);
    msg->fs_resp.op.read.count = ret;
    set_msg_readable(&msg->fs_resp.flag);
}

void handle_polling_fs_write(struct shm_msg *msg)
{
    int fd = msg->fs_req.op.write.fd;
    size_t count = msg->fs_req.op.write.count;
    void *buf = (void *)malloc(count);
    memcpy(buf, msg->fs_req.op.write.buf, count);
    set_msg_free(&msg->fs_req.flag);

    ssize_t ret = write(fd, buf, count);

    wait_msg_free(&msg->fs_resp.flag);
    msg->fs_resp.op.write.count = ret;
    set_msg_readable(&msg->fs_resp.flag);
    free(buf);
}

void handle_polling_fs_close(struct shm_msg *msg)
{
    int fd = msg->fs_req.op.close.fd;
    set_msg_free(&msg->fs_req.flag);

    int ret = close(fd);

    wait_msg_free(&msg->fs_resp.flag);
    msg->fs_resp.op.close.ret = ret;
    set_msg_readable(&msg->fs_resp.flag);
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

    wait_msg_readable(&msg->fs_resp.flag);
    int fd = msg->fs_resp.op.open.fd;
    set_msg_free(&msg->fs_resp.flag);

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

        wait_msg_readable(&msg->fs_resp.flag);
        ssize_t response_read_count = msg->fs_resp.op.read.count;
        memcpy(buf_ptr, msg->fs_resp.op.read.buf, response_read_count);
        set_msg_free(&msg->fs_resp.flag);

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

        wait_msg_readable(&msg->fs_resp.flag);
        ssize_t response_write_count = msg->fs_resp.op.write.count;
        set_msg_free(&msg->fs_resp.flag);

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

    wait_msg_readable(&msg->fs_resp.flag);
    int ret = msg->fs_resp.op.close.ret;
    set_msg_free(&msg->fs_resp.flag);

    return ret;
}