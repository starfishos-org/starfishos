#include "polling_server.h"
#include "polling_resp.h"
#include "polling_tp.h"

#include <chcore/syscall.h>
#include <chcore/memory.h>

#define USE_THREAD_POOL 0 // 0: single thread, 1: thread pool (buggy)

int my_id = -1;

void init_polling_shm_region(struct polling_shm_region *shm)
{
    atomic_init(&shm->write_index, 0);
    atomic_init(&shm->read_index, 0);
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        atomic_init(&shm->msgs[i].state, MSG_FREE);
        memset(&shm->msgs[i], 0, sizeof(struct shm_msg));
    }
}

void *polling_reader_thread(void *arg)
{
#if USE_THREAD_POOL == 1
    thread_pool_t pool;
    thread_pool_init(&pool);
#endif
    struct polling_shm_region *shm = arg;
    int r = atomic_load_explicit(&shm->read_index, memory_order_acquire);

    while (1) {
        struct shm_msg *msg = &shm->msgs[r % MAX_MSG_COUNT];

        int expected = MSG_REQ_READY;
        if (atomic_compare_exchange_strong_explicit(&msg->state,
                                                    &expected,
                                                    MSG_RESP_WRITING,
                                                    memory_order_acquire,
                                                    memory_order_relaxed)) {
#if USE_THREAD_POOL == 1
            thread_pool_add_task(&pool, msg);
#elif USE_THREAD_POOL == 0
            handle_polling_request(msg);
#endif

            r++;
            atomic_store_explicit(&shm->read_index, r, memory_order_relaxed);
        }
    }
}

void create_polling_thread(u32 shm_id, pthread_t *tid)
{
    int ret;
    void *shm_addr;
    shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    ret = usys_mmap_shm(shm_id, shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", shm_id);
        return;
    }
    printf("Polling Service Server: mmap shm by id %d\n", shm_id);
    struct polling_shm_region *shm = (struct polling_shm_region *)shm_addr;
    init_polling_shm_region(shm);
    ret = pthread_create(tid, NULL, polling_reader_thread, shm);
    if (ret != 0) {
        printf("Failed to create polling thread\n");
        return;
    }
}

int main(int argc, char *argv[])
{
    pthread_t tid;
    my_id = usys_get_machine_id();
    assert(my_id >= 0);
    create_polling_thread(my_id, &tid);
    pthread_join(tid, NULL);
    printf("Polling thread exited\n");
    return 0;
}