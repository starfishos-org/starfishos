#include "polling_req.h"

struct shm_msg *mpsc_alloc_msg(struct polling_shm_region *shm)
{
    while (1) {
        uint32_t idx = atomic_fetch_add_explicit(
                               &shm->write_index, 1, memory_order_relaxed)
                       % MAX_MSG_COUNT;

        struct shm_msg *msg = &shm->msgs[idx];

        int expected = MSG_FREE;
        if (atomic_compare_exchange_weak_explicit(&msg->state,
                                                  &expected,
                                                  MSG_REQ_WRITING,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            return msg;
        }
    }
}

void publish_request(struct shm_msg *msg, struct polling_fs_request *req)
{
    memcpy(&msg->fs_req, req, sizeof(struct polling_fs_request));
    atomic_store_explicit(&msg->state, MSG_REQ_READY, memory_order_release);
}

void polling_wait_for_response(struct shm_msg *msg)
{
    while (atomic_load_explicit(&msg->state, memory_order_acquire)
           != MSG_RESP_READY) {
    }
}

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode)
{
    struct polling_fs_request req = {
            .type = POLLING_FS_REQ_OPEN,
            .open =
                    {
                            .flags = flags,
                            .mode = mode,
                    },
    };
    strncpy(req.open.path, path, FS_REQ_PATH_BUF_LEN);

    struct shm_msg *msg = mpsc_alloc_msg(shm);
    publish_request(msg, &req);

    while (atomic_load_explicit(&msg->state, memory_order_acquire)
           != MSG_RESP_READY) {
    }

    int fd = msg->fs_resp.open.fd;

    atomic_store_explicit(&msg->state, MSG_FREE, memory_order_release);
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

        struct polling_fs_request req = {
                .type = POLLING_FS_REQ_READ,
                .read =
                        {
                                .fd = fd,
                                .count = chunk,
                        },
        };

        struct shm_msg *msg = mpsc_alloc_msg(shm);
        publish_request(msg, &req);

        while (atomic_load_explicit(&msg->state, memory_order_acquire)
               != MSG_RESP_READY) {
        }

        ssize_t n = msg->fs_resp.read.count;
        memcpy(p, msg->fs_resp.read.buf, n);

        atomic_store_explicit(&msg->state, MSG_FREE, memory_order_release);

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

        struct polling_fs_request req = {
                .type = POLLING_FS_REQ_WRITE,
                .write =
                        {
                                .fd = fd,
                                .count = chunk,
                        },
        };
        memcpy(req.write.buf, p, chunk);

        struct shm_msg *msg = mpsc_alloc_msg(shm);
        publish_request(msg, &req);

        while (atomic_load_explicit(&msg->state, memory_order_acquire)
               != MSG_RESP_READY) {
        }

        ssize_t n = msg->fs_resp.write.count;

        atomic_store_explicit(&msg->state, MSG_FREE, memory_order_release);

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
    struct polling_fs_request req = {
            .type = POLLING_FS_REQ_CLOSE,
            .close =
                    {
                            .fd = fd,
                    },
    };

    struct shm_msg *msg = mpsc_alloc_msg(shm);
    publish_request(msg, &req);

    while (atomic_load_explicit(&msg->state, memory_order_acquire)
           != MSG_RESP_READY) {
    }

    int ret = msg->fs_resp.close.ret;

    atomic_store_explicit(&msg->state, MSG_FREE, memory_order_release);
    return ret;
}

void polling_fs_empty(struct polling_shm_region *shm)
{
    struct polling_fs_request req = {
            .type = POLLING_FS_REQ_EMPTY,
    };
    struct shm_msg *msg = mpsc_alloc_msg(shm);

    publish_request(msg, &req);
    while (atomic_load_explicit(&msg->state, memory_order_acquire)
           != MSG_RESP_READY) {
    }

    atomic_store_explicit(&msg->state, MSG_FREE, memory_order_release);
}
