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

#define MAX_BATCH_OPS 1024 // Maximum batch operations (syscall limit)

/* Structure to pass arguments to polling thread */
struct polling_thread_arg {
    struct polling_shm_region *shm;
    int thread_id;
};

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
    struct polling_thread_arg *thread_arg = (struct polling_thread_arg *)arg;
    struct polling_shm_region *shm = thread_arg->shm;
    int thread_id = thread_arg->thread_id;
    
    /* Bind thread to a specific CPU from the configured list */
    /* Bind from the last CPU backwards */
    int target_cpu;
#if USE_THREAD_POOL == true
    /* Multiple threads: bind to CPUs from POLLING_CPU_LIST, starting from the last CPU */
    static const int polling_cpus[] = POLLING_CPU_LIST;
    int cpu_index = POLLING_CPU_COUNT - 1 - (thread_id % POLLING_CPU_COUNT);
    target_cpu = polling_cpus[cpu_index];
#else
    /* Single thread: use last CPU from the list */
    static const int polling_cpus[] = POLLING_CPU_LIST;
    target_cpu = polling_cpus[POLLING_CPU_COUNT - 1];
#endif
    
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(target_cpu, &cpu_set);
    if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
        printf("Failed to set polling thread %d affinity to CPU %d\n", thread_id, target_cpu);
    } else {
        printf("Polling thread %d bound to CPU %d\n", thread_id, target_cpu);
    }
    sched_yield(); /* Yield to ensure affinity takes effect */

    while (1) {
        int r;
        struct shm_msg *msg;
        
#if USE_THREAD_POOL == true
        /* Multi-threaded mode: try to get next message from queue */
        /* Each thread tries to claim the next available message */
        r = atomic_load_explicit(&shm->read_index, memory_order_acquire);
        int w = atomic_load_explicit(&shm->write_index, memory_order_acquire);
        
        /* Check if queue is empty */
        if (r >= w) {
            /* No messages available, yield and retry */
            sched_yield();
            continue;
        }
        
        msg = &shm->msgs[r % MAX_MSG_COUNT];
        
        /* Check if message is ready */
        int msg_state = atomic_load_explicit(&msg->state, memory_order_acquire);
        if (msg_state != MSG_REQ_READY) {
            /* Message not ready, yield and retry */
            sched_yield();
            continue;
        }
        
        /* Try to claim the message */
        int expected = MSG_REQ_READY;
        if (!atomic_compare_exchange_strong_explicit(&msg->state,
                                                     &expected,
                                                     MSG_RESP_WRITING,
                                                     memory_order_acquire,
                                                     memory_order_relaxed)) {
            /* Another thread got it first, yield and retry */
            sched_yield();
            continue;
        }
        
        /* Successfully claimed message, now update read_index */
        /* Use CAS to ensure only one thread updates it */
        int expected_r = r;
        if (!atomic_compare_exchange_strong_explicit(&shm->read_index,
                                                     &expected_r,
                                                     r + 1,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed)) {
            /* Another thread already updated read_index, but we got the message */
            /* This is fine, we can still process it */
        }
#else
        /* Single-threaded mode: use current read index */
        r = atomic_load_explicit(&shm->read_index, memory_order_acquire);
        msg = &shm->msgs[r % MAX_MSG_COUNT];
        
        int expected = MSG_REQ_READY;
        if (atomic_compare_exchange_strong_explicit(&msg->state,
                                                    &expected,
                                                    MSG_RESP_WRITING,
                                                    memory_order_acquire,
                                                    memory_order_relaxed)) {
#endif
#if USE_THREAD_POOL == false
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
#else /* USE_THREAD_POOL == true */
            /* Process request directly (multi-threaded mode) */
            handle_polling_request(msg);
            atomic_store_explicit(
                    &msg->state, MSG_RESP_READY, memory_order_release);
#endif
#if USE_THREAD_POOL == false
        } else {
            /* No message ready, yield to avoid busy waiting */
            sched_yield();
        }
#endif
    }
}

void create_polling_threads(u32 shm_id, pthread_t *tids, int num_threads)
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
    
    /* Allocate thread arguments */
    struct polling_thread_arg *thread_args = 
        (struct polling_thread_arg *)malloc(sizeof(struct polling_thread_arg) * num_threads);
    if (!thread_args) {
        printf("Failed to allocate memory for thread arguments\n");
        return;
    }
    
    /* Create multiple polling threads */
    for (int i = 0; i < num_threads; i++) {
        thread_args[i].shm = shm;
        thread_args[i].thread_id = i;
        ret = pthread_create(&tids[i], NULL, polling_reader_thread, &thread_args[i]);
        if (ret != 0) {
            printf("Failed to create polling thread %d\n", i);
            free(thread_args);
            return;
        }
    }
    
    /* Note: thread_args will be freed when threads exit (or we could track and free later) */
    /* For simplicity, we keep it allocated for the lifetime of the threads */
}

int main(int argc, char *argv[])
{
    my_id = usys_get_machine_id();
    assert(my_id >= 0);
    
#if USE_THREAD_POOL == true
    /* Create multiple polling threads */
    int num_threads = NUM_POLLING_THREADS;
    
    if (num_threads <= 0) {
        printf("NUM_POLLING_THREADS is %d, no polling threads will be created\n", num_threads);
        return 0;
    }
    
    pthread_t *tids = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    if (!tids) {
        printf("Failed to allocate memory for thread IDs\n");
        return -1;
    }
    
    printf("Creating %d polling threads\n", num_threads);
    create_polling_threads(my_id, tids, num_threads);
    
    /* Wait for all threads */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(tids[i], NULL);
    }
    
    free(tids);
    printf("All polling threads exited\n");
#else
    /* Single polling thread */
    pthread_t tid;
    create_polling_threads(my_id, &tid, 1);
    pthread_join(tid, NULL);
    printf("Polling thread exited\n");
#endif
    return 0;
}