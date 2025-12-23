#include "polling_resp.h"

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
    case POLLING_FS_REQ_EMPTY:
        handle_polling_fs_empty(msg);
        break;
    default:
        printf("Unsupported polling fs request type: %d\n", msg->fs_req.type);
        break;
    }
}

void handle_polling_fs_open(struct shm_msg *msg)
{
    char path[FS_REQ_PATH_BUF_LEN];
    int flags = msg->fs_req.open.flags;
    int mode = msg->fs_req.open.mode;
    strncpy(path, msg->fs_req.open.path, strlen(msg->fs_req.open.path));
    set_msg_free(&msg->fs_req_flag);

    int fd = open(path, flags, mode);

    wait_msg_free(&msg->fs_resp_flag);
    msg->fs_resp.open.fd = fd;
    set_msg_readable(&msg->fs_resp_flag);
}

void handle_polling_fs_read(struct shm_msg *msg)
{
    int fd = msg->fs_req.read.fd;
    size_t count = msg->fs_req.read.count;
    set_msg_free(&msg->fs_req_flag);

    ssize_t ret = read(fd, msg->fs_resp.read.buf, count);

    wait_msg_free(&msg->fs_resp_flag);
    msg->fs_resp.read.count = ret;
    set_msg_readable(&msg->fs_resp_flag);
}

void handle_polling_fs_write(struct shm_msg *msg)
{
    int fd = msg->fs_req.write.fd;
    size_t count = msg->fs_req.write.count;
    void *buf = (void *)malloc(count);
    memcpy(buf, msg->fs_req.write.buf, count);
    set_msg_free(&msg->fs_req_flag);

    ssize_t ret = write(fd, buf, count);

    wait_msg_free(&msg->fs_resp_flag);
    msg->fs_resp.write.count = ret;
    set_msg_readable(&msg->fs_resp_flag);
    free(buf);
}

void handle_polling_fs_close(struct shm_msg *msg)
{
    int fd = msg->fs_req.close.fd;
    set_msg_free(&msg->fs_req_flag);

    int ret = close(fd);

    wait_msg_free(&msg->fs_resp_flag);
    msg->fs_resp.close.ret = ret;
    set_msg_readable(&msg->fs_resp_flag);
}

void handle_polling_fs_empty(struct shm_msg *msg)
{
    set_msg_free(&msg->fs_req_flag);
    wait_msg_free(&msg->fs_resp_flag);
    set_msg_readable(&msg->fs_resp_flag);
}
