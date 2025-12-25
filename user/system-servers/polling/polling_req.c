#include "polling_req.h"
#include <stdatomic.h>

void debug_print_shm_region(struct polling_shm_region *shm)
{
    int w = atomic_load_explicit(&shm->write_index, memory_order_relaxed);
    int r = atomic_load_explicit(&shm->read_index, memory_order_acquire);
    printf("shm_region: write_index = %d, read_index = %d\n", w, r);
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        int state =
                atomic_load_explicit(&shm->msgs[i].state, memory_order_acquire);
        printf("shm_msg[%d]: state = %d\n", i, state);
    }
}

struct shm_msg *mpsc_alloc_msg_retry(struct polling_shm_region *shm)
{
    int w = atomic_load_explicit(&shm->write_index, memory_order_relaxed);
    int r = atomic_load_explicit(&shm->read_index, memory_order_acquire);

    if ((unsigned int)(w - r) >= MAX_MSG_COUNT - 1) {
        return NULL;
    }

    int idx = atomic_fetch_add_explicit(
            &shm->write_index, 1, memory_order_relaxed);
    struct shm_msg *msg = &shm->msgs[idx % MAX_MSG_COUNT];

    int expected = MSG_FREE;
    if (!atomic_compare_exchange_strong_explicit(&msg->state,
                                                 &expected,
                                                 MSG_REQ_WRITING,
                                                 memory_order_acquire,
                                                 memory_order_relaxed)) {
        return NULL;
    }

    return msg;
}

struct shm_msg *mpsc_alloc_msg(struct polling_shm_region *shm)
{
    while (1) {
        struct shm_msg *msg = mpsc_alloc_msg_retry(shm);
        if (msg != NULL) {
            return msg;
        }
    }
}

void polling_publish_request(struct shm_msg *msg, struct polling_request *req)
{
    memcpy(&msg->req, req, sizeof(struct polling_request));
    atomic_store_explicit(&msg->state, MSG_REQ_READY, memory_order_release);
}

void polling_wait_for_response(struct shm_msg *msg)
{
    while (atomic_load_explicit(&msg->state, memory_order_acquire)
           != MSG_RESP_READY) {
    }
}

void polling_free_msg(struct shm_msg *msg)
{
    atomic_store_explicit(&msg->state, MSG_FREE, memory_order_release);
}

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode)
{
    struct polling_request req = {
            .type = POLLING_FS_REQ_OPEN,
            .open =
                    {
                            .flags = flags,
                            .mode = mode,
                    },
    };
    strncpy(req.open.path, path, FS_REQ_PATH_BUF_LEN);

    struct shm_msg *msg = mpsc_alloc_msg(shm);
    polling_publish_request(msg, &req);

    polling_wait_for_response(msg);

    int fd = msg->resp.open.fd;

    polling_free_msg(msg);
    return fd;
}

ssize_t polling_fs_read(struct polling_shm_region *shm, int fd, void *buf,
                        size_t count)
{
    size_t left = count;
    char *p = buf;
    ssize_t total = 0;

    while (left > 0) {
        size_t chunk = MIN(left, POLLING_FS_READ_BUF_SIZE);

        struct polling_request req = {
                .type = POLLING_FS_REQ_READ,
                .read =
                        {
                                .fd = fd,
                                .count = chunk,
                        },
        };

        struct shm_msg *msg = mpsc_alloc_msg(shm);
        polling_publish_request(msg, &req);

        polling_wait_for_response(msg);

        ssize_t n = msg->resp.read.count;
        memcpy(p, msg->resp.read.buf, n);

        polling_free_msg(msg);

        total += n;
        p += n;
        left -= n;

        if (n < (ssize_t)chunk)
            break;
    }
    return total;
}

ssize_t polling_fs_write(struct polling_shm_region *shm, int fd,
                         const void *buf, size_t count)
{
    size_t left = count;
    const char *p = buf;
    ssize_t total = 0;

    while (left > 0) {
        size_t chunk = MIN(left, POLLING_FS_WRITE_BUF_SIZE);

        struct polling_request req = {
                .type = POLLING_FS_REQ_WRITE,
                .write =
                        {
                                .fd = fd,
                                .count = chunk,
                        },
        };
        memcpy(req.write.buf, p, chunk);

        struct shm_msg *msg = mpsc_alloc_msg(shm);
        polling_publish_request(msg, &req);

        polling_wait_for_response(msg);

        ssize_t n = msg->resp.write.count;

        polling_free_msg(msg);

        total += n;
        p += n;
        left -= n;

        if (n < (ssize_t)chunk)
            break;
    }
    return total;
}

int polling_fs_close(struct polling_shm_region *shm, int fd)
{
    struct polling_request req = {
            .type = POLLING_FS_REQ_CLOSE,
            .close =
                    {
                            .fd = fd,
                    },
    };

    struct shm_msg *msg = mpsc_alloc_msg(shm);
    polling_publish_request(msg, &req);

    polling_wait_for_response(msg);

    int ret = msg->resp.close.ret;

    polling_free_msg(msg);
    return ret;
}

void polling_fs_empty(struct polling_shm_region *shm)
{
    struct polling_request req = {
            .type = POLLING_REQ_EMPTY,
    };
    struct shm_msg *msg = mpsc_alloc_msg(shm);

    polling_publish_request(msg, &req);
    polling_wait_for_response(msg);

    polling_free_msg(msg);
}
