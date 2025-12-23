#include "polling_req.h"

void polling_enqueue_fs_request(struct shm_msg *msg,
                                struct polling_fs_request *req)
{
    // wait for the message slot to be free
    wait_msg_free(&msg->fs_req_flag);

    memcpy(&msg->fs_req, req, sizeof(struct polling_fs_request));
    set_msg_readable(&msg->fs_req_flag);
}

void polling_wait_for_response(struct shm_msg *msg)
{
    wait_msg_readable(&msg->fs_resp_flag);
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
    int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
    struct shm_msg *msg = &shm->msgs[idx];
    polling_enqueue_fs_request(msg, &req);

    wait_msg_readable(&msg->fs_resp_flag);
    int fd = msg->fs_resp.open.fd;
    set_msg_free(&msg->fs_resp_flag);

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
                .read =
                        {
                                .fd = fd,
                                .count = read_count,
                        },
        };
        int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
        struct shm_msg *msg = &shm->msgs[idx];
        polling_enqueue_fs_request(msg, &req);

        wait_msg_readable(&msg->fs_resp_flag);
        ssize_t response_read_count = msg->fs_resp.read.count;
        memcpy(buf_ptr, msg->fs_resp.read.buf, response_read_count);
        set_msg_free(&msg->fs_resp_flag);

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
                .write =
                        {
                                .fd = fd,
                                .count = write_count,
                        },
        };
        memcpy(req.write.buf, buf_ptr, write_count);
        int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
        struct shm_msg *msg = &shm->msgs[idx];
        polling_enqueue_fs_request(msg, &req);

        wait_msg_readable(&msg->fs_resp_flag);
        ssize_t response_write_count = msg->fs_resp.write.count;
        set_msg_free(&msg->fs_resp_flag);

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
            .close =
                    {
                            .fd = fd,
                    },
    };
    int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
    struct shm_msg *msg = &shm->msgs[idx];
    polling_enqueue_fs_request(msg, &req);

    wait_msg_readable(&msg->fs_resp_flag);
    int ret = msg->fs_resp.close.ret;
    set_msg_free(&msg->fs_resp_flag);

    return ret;
}

void polling_fs_empty(struct polling_shm_region *shm)
{
    struct polling_fs_request req = {
            .type = POLLING_FS_REQ_EMPTY,
    };
    int idx = atomic_fetch_add(&shm->write_index, 1) % MAX_MSG_COUNT;
    struct shm_msg *msg = &shm->msgs[idx];
    polling_enqueue_fs_request(msg, &req);
    wait_msg_readable(&msg->fs_resp_flag);
    set_msg_free(&msg->fs_resp_flag);
}
