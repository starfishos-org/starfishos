#include <chcore/memory.h>
#include <chcore/pci_ioctl.h>
#include <chcore/syscall.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "chcore_mman.h"
#include "hostfs.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define HOSTFS_HEARTBEAT_TIMEOUT_NS (5ULL * 1000 * 1000 * 1000)
#define HOSTFS_DIRENT_SIZE          280U

struct hostfs_context {
    void *mapping;
    u64 map_size;
    u32 slot_count;
    u32 slot_stride;
    u32 data_size;
    int error;
};

struct hostfs_response {
    s64 result;
    u64 handle;
    u64 offset;
    struct hostfs_metadata metadata;
};

static struct hostfs_context hostfs_ctx;
static pthread_once_t hostfs_once = PTHREAD_ONCE_INIT;
static volatile u32 hostfs_sequence;

_Static_assert(offsetof(struct hostfs_live_slot, result) == 16,
               "HostFS result offset mismatch");
_Static_assert(offsetof(struct hostfs_live_slot, path) == 128,
               "HostFS path offset mismatch");
_Static_assert(sizeof(struct hostfs_live_slot) <= HOSTFS_LIVE_SLOT_HEADER_SIZE,
               "HostFS slot header is too large");
_Static_assert(sizeof(struct dirent) == HOSTFS_DIRENT_SIZE,
               "HostFS dirent ABI mismatch");

static u64 monotonic_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (u64)now.tv_sec * 1000 * 1000 * 1000 + now.tv_nsec;
}

static void hostfs_connect_once(void)
{
    struct pci_control_req request = {0};
    struct pci_hostfs_connect_info info = {0};
    struct hostfs_live_header *header;
    void *mapping;
    int ret;

    request.req_type = PCI_CONTROL_IVSHMEM_CONNECT;
    request.arg_ptr = (u64)&info;
    request.arg_sz = sizeof(info);
    ret = usys_pcie_control((u64)&request);
    if (ret < 0) {
        hostfs_ctx.error = ret;
        return;
    }

    mapping = chcore_mmap(NULL,
                          info.map_size,
                          PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE,
                          -1,
                          0,
                          info.pmo_cap);
    if (mapping == MAP_FAILED) {
        hostfs_ctx.error = -ENOMEM;
        return;
    }

    header = mapping;
    if (memcmp(header->magic, HOSTFS_LIVE_MAGIC, 7) != 0
        || header->version != HOSTFS_LIVE_VERSION
        || header->slot_count != info.slot_count
        || header->slot_stride != info.slot_stride
        || header->data_size != info.data_size) {
        chcore_munmap(mapping, info.map_size);
        hostfs_ctx.error = -EPROTO;
        return;
    }

    hostfs_ctx.mapping = mapping;
    hostfs_ctx.map_size = info.map_size;
    hostfs_ctx.slot_count = info.slot_count;
    hostfs_ctx.slot_stride = info.slot_stride;
    hostfs_ctx.data_size = info.data_size;
}

static int hostfs_connect(void)
{
    pthread_once(&hostfs_once, hostfs_connect_once);
    if (!hostfs_ctx.mapping)
        return hostfs_ctx.error ? hostfs_ctx.error : -ENODEV;
    return 0;
}

static struct hostfs_live_slot *slot_at(u32 index)
{
    return (struct hostfs_live_slot *)((char *)hostfs_ctx.mapping
                                       + HOSTFS_LIVE_HEADER_SIZE
                                       + (u64)index * hostfs_ctx.slot_stride);
}

static int hostfs_check_server(u64 request_epoch, u64 *last_heartbeat,
                               u64 *heartbeat_deadline)
{
    struct hostfs_live_header *header = hostfs_ctx.mapping;
    u64 heartbeat;

    if (__atomic_load_n(&header->server_epoch, __ATOMIC_ACQUIRE)
        != request_epoch)
        return -ESTALE;
    heartbeat = __atomic_load_n(&header->heartbeat, __ATOMIC_ACQUIRE);
    if (heartbeat != *last_heartbeat) {
        *last_heartbeat = heartbeat;
        *heartbeat_deadline = monotonic_ns() + HOSTFS_HEARTBEAT_TIMEOUT_NS;
    }
    if (monotonic_ns() >= *heartbeat_deadline)
        return -EIO;
    return 0;
}

static void response_from_slot(struct hostfs_response *response,
                               struct hostfs_live_slot *slot)
{
    if (!response)
        return;
    response->result = slot->result;
    response->handle = slot->handle;
    response->offset = slot->offset;
    response->metadata.size = slot->size;
    response->metadata.mode = slot->mode;
    response->metadata.ino = slot->ino;
    response->metadata.atime_ns = slot->atime_ns;
    response->metadata.mtime_ns = slot->mtime_ns;
    response->metadata.ctime_ns = slot->ctime_ns;
    response->metadata.uid = slot->uid;
    response->metadata.gid = slot->gid;
    response->metadata.nlink = slot->nlink;
    response->metadata.blocks = slot->blocks;
    response->metadata.block_size = slot->block_size;
}

static s64 hostfs_submit(enum hostfs_live_opcode opcode, const char *path,
                         u64 handle, u32 flags, u64 offset, u64 mode,
                         const void *input, size_t input_size, void *output,
                         size_t output_size, struct hostfs_response *response)
{
    struct hostfs_live_header *header;
    struct hostfs_live_slot *slot = NULL;
    u64 heartbeat_deadline;
    u64 last_heartbeat;
    u64 request_count;
    u64 request_epoch;
    u32 spins = 0;
    u32 index;
    int ret;

    ret = hostfs_connect();
    if (ret < 0)
        return ret;
    if (input_size > hostfs_ctx.data_size || output_size > hostfs_ctx.data_size)
        return -E2BIG;
    if (path && strlen(path) >= HOSTFS_LIVE_PATH_SIZE)
        return -ENAMETOOLONG;

    header = hostfs_ctx.mapping;
    if (memcmp(header->magic, HOSTFS_LIVE_MAGIC, 7) != 0
        || header->version != HOSTFS_LIVE_VERSION)
        return -ESTALE;
    request_epoch = __atomic_load_n(&header->server_epoch, __ATOMIC_ACQUIRE);
    if (request_epoch == 0)
        return -ESTALE;
    last_heartbeat = __atomic_load_n(&header->heartbeat, __ATOMIC_ACQUIRE);
    heartbeat_deadline = monotonic_ns() + HOSTFS_HEARTBEAT_TIMEOUT_NS;
    while (!slot) {
        for (index = 0; index < hostfs_ctx.slot_count; ++index) {
            struct hostfs_live_slot *candidate = slot_at(index);
            if (__sync_bool_compare_and_swap(&candidate->state,
                                             HOSTFS_SLOT_FREE,
                                             HOSTFS_SLOT_CLAIMED)) {
                slot = candidate;
                break;
            }
        }
        if (slot)
            break;
        ret = hostfs_check_server(
                request_epoch, &last_heartbeat, &heartbeat_deadline);
        if (ret < 0)
            return ret;
        usys_yield();
    }

    memset((char *)slot + sizeof(slot->state),
           0,
           HOSTFS_LIVE_SLOT_HEADER_SIZE - sizeof(slot->state));
    slot->sequence = __sync_add_and_fetch(&hostfs_sequence, 1);
    slot->opcode = opcode;
    slot->flags = flags;
    slot->handle = handle;
    slot->offset = offset;
    slot->mode = mode;
    request_count = input_size ? input_size : output_size;
    slot->count = request_count;
    if (path) {
        slot->path_length = strlen(path);
        memcpy(slot->path, path, slot->path_length);
    }
    if (input_size) {
        void *data = (char *)slot + HOSTFS_LIVE_SLOT_HEADER_SIZE;
        memcpy(data, input, input_size);
    }

    __atomic_store_n(&slot->state, HOSTFS_SLOT_READY, __ATOMIC_RELEASE);
    while (__atomic_load_n(&slot->state, __ATOMIC_ACQUIRE)
           != HOSTFS_SLOT_DONE) {
        ret = hostfs_check_server(
                request_epoch, &last_heartbeat, &heartbeat_deadline);
        if (ret < 0)
            return ret;
        if (++spins >= 256) {
            spins = 0;
            usys_yield();
        }
    }

    response_from_slot(response, slot);
    ret = slot->result;
    if (ret > 0 && output && output_size) {
        size_t copy_size = MIN((size_t)ret, output_size);
        void *data = (char *)slot + HOSTFS_LIVE_SLOT_HEADER_SIZE;
        memcpy(output, data, copy_size);
    }
    __atomic_store_n(&slot->state, HOSTFS_SLOT_FREE, __ATOMIC_RELEASE);
    return ret;
}

static struct hostfs_file_info *hostfs_info(int fd)
{
    if (fd < 0 || fd >= MAX_FD || !fd_dic[fd]
        || fd_dic[fd]->type != FD_TYPE_HOSTFS)
        return NULL;
    return fd_dic[fd]->private_data;
}

const char *chcore_hostfs_path(int fd)
{
    struct hostfs_file_info *info = hostfs_info(fd);

    return info ? info->file_name : NULL;
}

static int chcore_hostfs_fcntl(int fd, int command, int argument)
{
    struct hostfs_response response;
    struct hostfs_file_info *info = hostfs_info(fd);
    void *old_private;
    s64 ret;
    int new_fd;

    if (!info)
        return -EBADF;
    switch (command) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        new_fd = alloc_fd_since(argument);
        if (new_fd < 0)
            return new_fd;
        old_private = fd_dic[new_fd]->private_data;
        fd_dic[new_fd]->type = FD_TYPE_HOSTFS;
        fd_dic[new_fd]->fd_op = &hostfs_ops;
        fd_dic[new_fd]->flags = command == F_DUPFD_CLOEXEC ? FD_CLOEXEC : 0;
        fd_dic[new_fd]->private_data = info;
        __sync_add_and_fetch(&info->refcount, 1);
        free(old_private);
        return new_fd;
    case F_GETFL:
    case F_SETFL:
        pthread_mutex_lock(&info->lock);
        ret = hostfs_submit(HOSTFS_OP_FCNTL,
                            NULL,
                            info->handle,
                            argument,
                            0,
                            command,
                            NULL,
                            0,
                            NULL,
                            0,
                            &response);
        if (ret < 0) {
            pthread_mutex_unlock(&info->lock);
            return ret;
        }
        if (command == F_GETFL) {
            info->flags = ret;
            pthread_mutex_unlock(&info->lock);
            return ret;
        }
        info->flags = (info->flags & O_ACCMODE) | argument;
        pthread_mutex_unlock(&info->lock);
        return 0;
    default:
        return -ENOTSUP;
    }
}

static void metadata_to_stat(const struct hostfs_metadata *metadata,
                             struct stat *statbuf)
{
    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->st_dev = 0x48465332;
    statbuf->st_ino = metadata->ino;
    statbuf->st_mode = metadata->mode;
    statbuf->st_nlink = metadata->nlink;
    statbuf->st_uid = metadata->uid;
    statbuf->st_gid = metadata->gid;
    statbuf->st_size = metadata->size;
    statbuf->st_blksize = metadata->block_size;
    statbuf->st_blocks = metadata->blocks;
    statbuf->st_atim.tv_sec = metadata->atime_ns / 1000000000ULL;
    statbuf->st_atim.tv_nsec = metadata->atime_ns % 1000000000ULL;
    statbuf->st_mtim.tv_sec = metadata->mtime_ns / 1000000000ULL;
    statbuf->st_mtim.tv_nsec = metadata->mtime_ns % 1000000000ULL;
    statbuf->st_ctim.tv_sec = metadata->ctime_ns / 1000000000ULL;
    statbuf->st_ctim.tv_nsec = metadata->ctime_ns % 1000000000ULL;
}

int chcore_hostfs_open(int fd, const char *path, int flags, mode_t mode)
{
    struct hostfs_response response;
    struct hostfs_file_info *info;
    void *old_private;
    s64 ret;

    ret = hostfs_submit(
            HOSTFS_OP_OPEN, path, 0, flags, 0, mode, NULL, 0, NULL, 0, &response);
    if (ret < 0) {
        if (fd >= 0 && fd < MAX_FD && fd_dic[fd]) {
            free(fd_dic[fd]->private_data);
            fd_dic[fd]->private_data = NULL;
            free_fd(fd);
        }
        return ret;
    }

    info = calloc(1, sizeof(*info));
    if (!info) {
        hostfs_submit(HOSTFS_OP_CLOSE,
                      NULL,
                      response.handle,
                      0,
                      0,
                      0,
                      NULL,
                      0,
                      NULL,
                      0,
                      NULL);
        free(fd_dic[fd]->private_data);
        fd_dic[fd]->private_data = NULL;
        free_fd(fd);
        return -ENOMEM;
    }
    info->handle = response.handle;
    info->file_size = response.metadata.size;
    info->mode = response.metadata.mode;
    info->flags = flags;
    info->refcount = 1;
    strncpy(info->file_name, path, sizeof(info->file_name) - 1);
    pthread_mutex_init(&info->lock, NULL);

    old_private = fd_dic[fd]->private_data;
    fd_dic[fd]->type = FD_TYPE_HOSTFS;
    fd_dic[fd]->fd_op = &hostfs_ops;
    fd_dic[fd]->flags = flags & O_CLOEXEC ? FD_CLOEXEC : 0;
    fd_dic[fd]->private_data = info;
    free(old_private);
    return fd;
}

int chcore_hostfs_pread(int fd, void *buf, size_t count, off_t offset)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    size_t completed = 0;
    s64 ret = 0;

    if (!info)
        return -EBADF;
    if (offset < 0)
        return -EINVAL;
    while (completed < count) {
        size_t chunk = MIN(count - completed, hostfs_ctx.data_size);
        ret = hostfs_submit(HOSTFS_OP_PREAD,
                            NULL,
                            info->handle,
                            0,
                            offset + completed,
                            0,
                            NULL,
                            0,
                            (char *)buf + completed,
                            chunk,
                            &response);
        if (ret < 0)
            return completed ? (int)completed : (int)ret;
        info->file_size = response.metadata.size;
        completed += ret;
        if ((size_t)ret != chunk)
            break;
    }
    return completed;
}

int chcore_hostfs_read(int fd, void *buf, size_t count)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    int ret;

    if (!info)
        return -EBADF;
    pthread_mutex_lock(&info->lock);
    ret = chcore_hostfs_pread(fd, buf, count, info->fd_offset);
    if (ret > 0)
        info->fd_offset += ret;
    pthread_mutex_unlock(&info->lock);
    return ret;
}

static int hostfs_pwrite_internal(int fd, void *buf, size_t count, off_t offset,
                                  bool append, off_t *final_offset)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    size_t completed = 0;
    s64 ret = 0;

    if (!info)
        return -EBADF;
    if (offset < 0)
        return -EINVAL;
    while (completed < count) {
        size_t chunk = MIN(count - completed, hostfs_ctx.data_size);
        ret = hostfs_submit(HOSTFS_OP_PWRITE,
                            NULL,
                            info->handle,
                            append ? HOSTFS_IO_APPEND : 0,
                            offset + completed,
                            0,
                            (char *)buf + completed,
                            chunk,
                            NULL,
                            0,
                            &response);
        if (ret < 0)
            return completed ? (int)completed : (int)ret;
        info->file_size = response.metadata.size;
        if (append && final_offset)
            *final_offset = response.offset;
        completed += ret;
        if ((size_t)ret != chunk)
            break;
    }
    return completed;
}

int chcore_hostfs_pwrite(int fd, void *buf, size_t count, off_t offset)
{
    return hostfs_pwrite_internal(fd, buf, count, offset, false, NULL);
}

int chcore_hostfs_write(int fd, void *buf, size_t count)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    off_t final_offset;
    bool append;
    int ret;

    if (!info)
        return -EBADF;
    pthread_mutex_lock(&info->lock);
    append = info->flags & O_APPEND;
    final_offset = info->fd_offset;
    ret = hostfs_pwrite_internal(
            fd, buf, count, info->fd_offset, append, &final_offset);
    if (ret > 0)
        info->fd_offset = append ? final_offset : info->fd_offset + ret;
    pthread_mutex_unlock(&info->lock);
    return ret;
}

off_t chcore_hostfs_lseek(int fd, off_t offset, int whence)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    off_t new_offset;
    s64 ret;

    if (!info)
        return -EBADF;
    pthread_mutex_lock(&info->lock);
    if (whence == SEEK_SET) {
        new_offset = offset;
    } else if (whence == SEEK_CUR) {
        new_offset = info->fd_offset + offset;
    } else if (whence == SEEK_END) {
        ret = hostfs_submit(HOSTFS_OP_FSTAT,
                            NULL,
                            info->handle,
                            0,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            0,
                            &response);
        if (ret < 0) {
            pthread_mutex_unlock(&info->lock);
            return ret;
        }
        info->file_size = response.metadata.size;
        new_offset = info->file_size + offset;
    } else {
        pthread_mutex_unlock(&info->lock);
        return -EINVAL;
    }
    if (new_offset < 0) {
        pthread_mutex_unlock(&info->lock);
        return -EINVAL;
    }
    info->fd_offset = new_offset;
    pthread_mutex_unlock(&info->lock);
    return new_offset;
}

int chcore_hostfs_close(int fd)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    s64 ret;

    if (!info)
        return -EBADF;
    fd_dic[fd]->private_data = NULL;
    free_fd(fd);
    if (__sync_sub_and_fetch(&info->refcount, 1) != 0)
        return 0;
    ret = hostfs_submit(
            HOSTFS_OP_CLOSE, NULL, info->handle, 0, 0, 0, NULL, 0, NULL, 0, NULL);
    pthread_mutex_destroy(&info->lock);
    free(info);
    return ret == -EBADF ? 0 : ret;
}

u64 chcore_hostfs_mmap(u64 vaddr, size_t length, int prot, int flags, int fd,
                       off_t offset)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    void *buffer;
    void *mapping;
    size_t completed = 0;
    int map_flags;
    int pmo_cap;
    int ret;

    (void)vaddr;
    if (!info || offset < 0)
        return (u64)-EINVAL;
    if (!(flags & MAP_PRIVATE) || (flags & MAP_SHARED))
        return (u64)-ENOTSUP;
    if (length == 0)
        return (u64)-EINVAL;

    pmo_cap = usys_create_pmo(
            ROUND_UP(length, PAGE_SIZE), PMO_DATA, MALLOC_TYPE_DEFAULT);
    if (pmo_cap < 0)
        return pmo_cap;
    buffer = malloc(MIN(length, hostfs_ctx.data_size));
    if (!buffer) {
        usys_revoke_cap(pmo_cap);
        return (u64)-ENOMEM;
    }
    while (completed < length) {
        size_t chunk = MIN(length - completed, hostfs_ctx.data_size);
        int bytes_read =
                chcore_hostfs_pread(fd, buffer, chunk, offset + completed);
        if (bytes_read < 0) {
            ret = bytes_read;
            goto error;
        }
        if (bytes_read == 0)
            break;
        ret = usys_write_pmo(pmo_cap, completed, buffer, bytes_read);
        if (ret < 0)
            goto error;
        completed += bytes_read;
        if ((size_t)bytes_read != chunk)
            break;
    }
    free(buffer);
    map_flags = MAP_ANONYMOUS | MAP_PRIVATE
                | (flags & (MAP_FLAG_SHARED | MAP_FLAG_PRIVATE));
    mapping = chcore_mmap(NULL, length, prot, map_flags, -1, 0, pmo_cap);
    if (mapping == MAP_FAILED)
        return (u64)-ENOMEM;
    return (u64)mapping;

error:
    free(buffer);
    usys_revoke_cap(pmo_cap);
    return (u64)ret;
}

int chcore_hostfs_stat(int fd, const char *path, int flags,
                       struct stat *statbuf, size_t bufsize)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    s64 ret;

    if (!statbuf || bufsize < sizeof(*statbuf))
        return -EINVAL;
    if (info)
        ret = hostfs_submit(HOSTFS_OP_FSTAT,
                            NULL,
                            info->handle,
                            flags,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            0,
                            &response);
    else if (path)
        ret = hostfs_submit(HOSTFS_OP_STAT,
                            path,
                            0,
                            flags,
                            0,
                            0,
                            NULL,
                            0,
                            NULL,
                            0,
                            &response);
    else
        return -EBADF;
    if (ret < 0)
        return ret;
    if (info) {
        info->file_size = response.metadata.size;
        info->mode = response.metadata.mode;
    }
    metadata_to_stat(&response.metadata, statbuf);
    return 0;
}

int chcore_hostfs_statfs(struct statfs *statbuf)
{
    struct hostfs_response response;
    s64 ret;

    if (!statbuf)
        return -EINVAL;
    ret = hostfs_submit(
            HOSTFS_OP_STATFS, NULL, 0, 0, 0, 0, NULL, 0, NULL, 0, &response);
    if (ret < 0)
        return ret;
    memset(statbuf, 0, sizeof(*statbuf));
    statbuf->f_type = 0x48465332;
    statbuf->f_bsize = response.metadata.block_size;
    statbuf->f_blocks = response.metadata.size;
    statbuf->f_bfree = response.metadata.mode;
    statbuf->f_bavail = response.metadata.ino;
    statbuf->f_files = response.metadata.atime_ns;
    statbuf->f_ffree = response.metadata.mtime_ns;
    statbuf->f_namelen = response.metadata.nlink;
    statbuf->f_frsize = response.metadata.blocks;
    return 0;
}

int chcore_hostfs_fsync(int fd)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    if (!info)
        return -EBADF;
    return hostfs_submit(
            HOSTFS_OP_FSYNC, NULL, info->handle, 0, 0, 0, NULL, 0, NULL, 0, NULL);
}

int chcore_hostfs_ftruncate(int fd, off_t length)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    s64 ret;

    if (!info)
        return -EBADF;
    if (length < 0)
        return -EINVAL;
    ret = hostfs_submit(HOSTFS_OP_FTRUNCATE,
                        NULL,
                        info->handle,
                        0,
                        length,
                        0,
                        NULL,
                        0,
                        NULL,
                        0,
                        &response);
    if (ret >= 0)
        info->file_size = response.metadata.size;
    return ret;
}

int chcore_hostfs_fallocate(int fd, int mode, off_t offset, off_t length)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    s64 ret;

    if (!info)
        return -EBADF;
    if (offset < 0 || length <= 0)
        return -EINVAL;
    ret = hostfs_submit(HOSTFS_OP_FALLOCATE,
                        NULL,
                        info->handle,
                        mode,
                        offset,
                        length,
                        NULL,
                        0,
                        NULL,
                        0,
                        &response);
    if (ret >= 0)
        info->file_size = response.metadata.size;
    return ret;
}

int chcore_hostfs_getdents64(int fd, char *buf, int count)
{
    struct hostfs_file_info *info = hostfs_info(fd);
    struct hostfs_response response;
    s64 ret;

    if (!info)
        return -EBADF;
    if (count < (int)HOSTFS_DIRENT_SIZE)
        return -EINVAL;
    pthread_mutex_lock(&info->lock);
    ret = hostfs_submit(HOSTFS_OP_GETDENTS,
                        NULL,
                        info->handle,
                        0,
                        info->fd_offset,
                        0,
                        NULL,
                        0,
                        buf,
                        MIN((u32)count, hostfs_ctx.data_size),
                        &response);
    if (ret >= 0)
        info->fd_offset = response.offset;
    pthread_mutex_unlock(&info->lock);
    return ret;
}

int chcore_hostfs_mkdir(const char *path, mode_t mode)
{
    return hostfs_submit(
            HOSTFS_OP_MKDIR, path, 0, 0, 0, mode, NULL, 0, NULL, 0, NULL);
}

int chcore_hostfs_unlink(const char *path, int flags)
{
    enum hostfs_live_opcode opcode = flags & AT_REMOVEDIR ? HOSTFS_OP_RMDIR :
                                                            HOSTFS_OP_UNLINK;
    return hostfs_submit(opcode, path, 0, flags, 0, 0, NULL, 0, NULL, 0, NULL);
}

int chcore_hostfs_rename(const char *old_path, const char *new_path)
{
    return hostfs_submit(HOSTFS_OP_RENAME,
                         old_path,
                         0,
                         0,
                         0,
                         0,
                         new_path,
                         strlen(new_path),
                         NULL,
                         0,
                         NULL);
}

int chcore_hostfs_access(const char *path, int mode, int flags)
{
    return hostfs_submit(
            HOSTFS_OP_ACCESS, path, 0, flags, 0, mode, NULL, 0, NULL, 0, NULL);
}

int chcore_hostfs_readlink(const char *path, char *buf, size_t bufsiz)
{
    int ret = hostfs_connect();
    if (ret < 0)
        return ret;
    return hostfs_submit(HOSTFS_OP_READLINK,
                         path,
                         0,
                         0,
                         0,
                         0,
                         NULL,
                         0,
                         buf,
                         MIN(bufsiz, hostfs_ctx.data_size),
                         NULL);
}

int chcore_hostfs_symlink(const char *target, const char *link_path)
{
    return hostfs_submit(HOSTFS_OP_SYMLINK,
                         link_path,
                         0,
                         0,
                         0,
                         0,
                         target,
                         strlen(target),
                         NULL,
                         0,
                         NULL);
}

struct fd_ops hostfs_ops = {
        .read = chcore_hostfs_read,
        .pread = chcore_hostfs_pread,
        .write = chcore_hostfs_write,
        .pwrite = chcore_hostfs_pwrite,
        .fcntl = chcore_hostfs_fcntl,
        .lseek = chcore_hostfs_lseek,
        .close = chcore_hostfs_close,
        .mmap = chcore_hostfs_mmap,
};
