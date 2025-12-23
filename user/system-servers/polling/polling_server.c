#include "polling_server.h"
#include "polling_resp.h"

#include <chcore/syscall.h>
#include <chcore/memory.h>

void init_polling_shm_region(struct polling_shm_region *shm)
{
    atomic_init(&shm->write_index, 0);
    atomic_init(&shm->read_index, 0);
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        set_msg_free(&shm->msgs[i].fs_req_flag);
        set_msg_free(&shm->msgs[i].fs_resp_flag);
    }
}

// support for **one** reader
void *polling_reader_thread(void *arg)
{
    struct polling_server_ctx *ctx = arg;
    struct polling_shm_region *shm = ctx->shm;
    struct shm_msg *msg;

    uint32_t read_idx =
            atomic_load(&shm->read_index);

    while (1) {
        uint32_t idx = read_idx % MAX_MSG_COUNT;
        msg = &shm->msgs[idx];

        if (atomic_load(&msg->fs_req_flag)
            == SHM_MSG_READABLE) {
            handle_polling_fs_request(msg);
            read_idx++;
            atomic_store(
                    &shm->read_index, read_idx);
        } else {
        }
    }

    return NULL;
}

void create_polling_thread(u32 shm_id, pthread_t *tid)
{
    int ret;
    struct polling_server_ctx *ctx;
    void *shm_addr;
    ctx = (struct polling_server_ctx *)malloc(
            sizeof(struct polling_server_ctx));
    if (ctx == NULL) {
        printf("Failed to allocate memory for ctx\n");
        return;
    }
    shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    ret = usys_mmap_shm(shm_id, shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", shm_id);
        return;
    }
    ctx->shm = (struct polling_shm_region *)shm_addr;
    init_polling_shm_region(ctx->shm);
    ret = pthread_create(tid, NULL, polling_reader_thread, ctx);
    if (ret != 0) {
        printf("Failed to create polling thread\n");
        return;
    }
}

int main(int argc, char *argv[])
{
    pthread_t tid;
    create_polling_thread(POLLING_FS_SHM_ID, &tid);
    pthread_join(tid, NULL);
    printf("Polling thread exited\n");
    return 0;
}