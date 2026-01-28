#define _GNU_SOURCE
#include "polling_server.h"
#include "polling_resp.h"
#include "polling_tp.h"
#include "polling_config.h"

#include <chcore/syscall.h>
#include <chcore/memory.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define USE_THREAD_POOL 0 // 0: single thread, 1: thread pool (buggy)
#define MAX_BATCH_OPS 1024 // Maximum batch operations (syscall limit)

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
    /* Bind this thread to the last CPU */
    int last_cpu = usys_get_machine_cpu_count() - 1;
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(last_cpu, &cpu_set);
    if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
        printf("Failed to set polling thread affinity to CPU %d\n", last_cpu);
    } else {
        printf("Polling thread bound to CPU %d\n", last_cpu);
    }
    sched_yield(); /* Yield to ensure affinity takes effect */

#if USE_THREAD_POOL == true
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
#if USE_THREAD_POOL == true
            thread_pool_add_task(&pool, msg);
#elif USE_THREAD_POOL == false
            /* Check if this is a flush_tlb request - if so, batch process */
            if (msg->req.type == POLLING_KERNEL_REQ_FLUSH_TLB) {
                /* Collect all flush_tlb requests from queue */
                struct shm_msg *batch_msgs[MAX_BATCH_OPS];
                struct memcpy_flush_tlb_op *ops_buf;
                int batch_count = 0;
                int w = atomic_load_explicit(&shm->write_index, memory_order_acquire);
                
                /* First, add the current message */
                batch_msgs[batch_count++] = msg;
                
                /* Scan queue for more flush_tlb requests */
                for (int i = r + 1; i < w && batch_count < MAX_BATCH_OPS; i++) {
                    struct shm_msg *candidate = &shm->msgs[i % MAX_MSG_COUNT];
                    int candidate_state = atomic_load_explicit(&candidate->state, memory_order_acquire);
                    
                    if (candidate_state == MSG_REQ_READY &&
                        candidate->req.type == POLLING_KERNEL_REQ_FLUSH_TLB) {
                        int candidate_expected = MSG_REQ_READY;
                        if (atomic_compare_exchange_strong_explicit(&candidate->state,
                                                                    &candidate_expected,
                                                                    MSG_RESP_WRITING,
                                                                    memory_order_acquire,
                                                                    memory_order_relaxed)) {
                            batch_msgs[batch_count++] = candidate;
                        }
                    }
                }
                
                /* Allocate buffer for batch operations */
                ops_buf = (struct memcpy_flush_tlb_op *)malloc(sizeof(struct memcpy_flush_tlb_op) * batch_count);
                if (!ops_buf) {
                    /* Fallback: process messages individually */
                    /* Restore state for collected messages (except the first one) */
                    for (int i = 1; i < batch_count; i++) {
                        atomic_store_explicit(&batch_msgs[i]->state, MSG_REQ_READY, memory_order_release);
                    }
                    /* Process current message individually */
                    handle_polling_request(msg);
                    atomic_store_explicit(&msg->state, MSG_RESP_READY, memory_order_release);
                    r++;
                    atomic_store_explicit(&shm->read_index, r, memory_order_relaxed);
                    continue;
                }
                
                /* Build operations array from collected messages */
                for (int i = 0; i < batch_count; i++) {
                    struct polling_kernel_req_flush_tlb *req = &batch_msgs[i]->req.flush_tlb;
                    ops_buf[i].src_pa = req->memcpy_src_pa;
                    ops_buf[i].dst_pa = req->memcpy_dst_pa;
                    ops_buf[i].len = req->memcpy_len;
                    ops_buf[i].fault_va = req->memcpy_fault_va;
                    ops_buf[i].vmspace_ptr = req->memcpy_vmspace;
                }
                
                /* Batch process all operations */
                int batch_ret = usys_memcpy_and_flush_tlb_batch(ops_buf, batch_count);
                
                /* Set response for all messages */
                assert(my_id >= 0); /* Ensure my_id is initialized */
                for (int i = 0; i < batch_count; i++) {
                    batch_msgs[i]->resp.flush_tlb.reply_result = batch_ret;
                    batch_msgs[i]->resp.flush_tlb.reply_from = my_id;
                    batch_msgs[i]->resp.flush_tlb.reply_received = 1;
                    atomic_store_explicit(&batch_msgs[i]->state, MSG_RESP_READY, memory_order_release);
                }
                
                free(ops_buf);
                
                /* Update read_index to skip all processed messages */
                r += batch_count;
                atomic_store_explicit(&shm->read_index, r, memory_order_relaxed);
            } else {
                /* Non-flush_tlb requests: process normally */
                atomic_store_explicit(
                        &msg->state, MSG_RESP_WRITING, memory_order_relaxed);
                handle_polling_request(msg);
                atomic_store_explicit(
                        &msg->state, MSG_RESP_READY, memory_order_release);
                r++;
                atomic_store_explicit(&shm->read_index, r, memory_order_relaxed);
            }
#endif
        } else {
            /* No message ready, yield to avoid busy waiting */
            sched_yield();
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