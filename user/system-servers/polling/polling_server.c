#define _GNU_SOURCE
#include "polling_server.h"
#include "polling_req.h"
#include "polling_resp.h"
#include "polling_tp.h"
#include "polling_config.h"

#include <chcore/syscall.h>
#include <chcore/memory.h>
#include <chcore-internal/fs_defs.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Server-side timing control ----
 * Set to 1 to collect and dump per-request timing data (dequeue + handle).
 * Dumped via polling_print_debug_info() when client sends POLLING_PRINT_DEBUG_INFO.
 */
#define ENABLE_SRV_TIMING 0

static inline long diff_ns(struct timespec a, struct timespec b)
{
    return (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
}

struct polling_thread_arg {
    struct polling_shm_region *shm;
    int thread_id;
};

int my_id = -1;

/* ================================================================
 * SHM region initialization
 *
 * Layout: [ durable_queue | dq_allocator | node[0](sentinel) | node[1..N-1](free) ]
 * ================================================================ */

void init_polling_shm_region(struct polling_shm_region *shm)
{
    int32_t pool_off = DQ_POOL_OFFSET;
    int32_t node_size = DQ_NODE_SIZE;
    int32_t max_nodes = DQ_MAX_NODES;

    /* Set up allocator metadata */
    shm->alloc.node_size = node_size;
    shm->alloc.node_count = max_nodes;
    shm->alloc.pool_offset = pool_off;

    /* Node 0 is the initial sentinel (dummy head) */
    qptr_t sentinel_off = pool_off;
    struct dq_node *sentinel = qptr_to_ptr(shm, sentinel_off);
    atomic_init(&sentinel->next, QPTR_NULL);
    atomic_init(&sentinel->status, DQ_CONSUMED); /* sentinel never used by producer */
    memset(&sentinel->req, 0, sizeof(struct polling_request));

    /* Initialize queue: head = tail = sentinel */
    atomic_init(&shm->queue.head, sentinel_off);
    atomic_init(&shm->queue.tail, sentinel_off);

    /* Build free list from node[1..max_nodes-1] (reverse order for LIFO) */
    atomic_init(&shm->alloc.free_list, QPTR_NULL);
    for (int i = max_nodes - 1; i >= 1; i--) {
        qptr_t off = pool_off + i * node_size;
        struct dq_node *node = qptr_to_ptr(shm, off);
        atomic_init(&node->status, DQ_FREE);
        atomic_init(&node->next, atomic_load(&shm->alloc.free_list));
        atomic_store(&shm->alloc.free_list, off);
    }

    printf("[dq_init] pool_off=%d node_size=%d max_nodes=%d\n",
           pool_off, node_size, max_nodes);
}

/* ================================================================
 * Dequeue — Algorithm from docs/durable-queue.md
 *
 *   while true:
 *     first <- head; last <- tail; next <- first->next
 *     if first == head:
 *       if first == last:
 *         if next == NULL: return NULL
 *         FLUSH(&last->next); CAS(&tail, last, next)
 *       else:
 *         n <- next
 *         if CAS(&n->status, INIT, DOING):
 *           FLUSH(&n->status)
 *           handle(n)
 *           n->status <- DONE; FLUSH(&n->status)
 *         CAS(&head, first, next)
 *         return
 *
 * After advancing head past the old sentinel, the old sentinel
 * is recycled to the free list (deferred by one step).
 * ================================================================ */

/* ================================================================
 * Server-side per-request timing (global, dumped on debug_info)
 * ================================================================ */

#define SRV_PERF_MAX 50000
static long srv_t_deq[SRV_PERF_MAX];
static long srv_t_handle[SRV_PERF_MAX];
static int srv_perf_idx = 0;

/* Called by handle_polling_print_debug_info via polling_resp.c */
void srv_dump_timing(void)
{
    printf("[SRV_TIMING_BEGIN] count=%d\n", srv_perf_idx);
    for (int i = 0; i < srv_perf_idx; i++)
        printf("[ST] %ld %ld\n", srv_t_deq[i], srv_t_handle[i]);
    printf("[SRV_TIMING_END]\n");
    srv_perf_idx = 0; /* reset for next run */
}

/* ================================================================
 * Polling thread — dequeue loop
 * ================================================================ */

void *polling_reader_thread(void *arg)
{
    struct polling_thread_arg *thread_arg = (struct polling_thread_arg *)arg;
    struct polling_shm_region *shm = thread_arg->shm;
    int thread_id = thread_arg->thread_id;

    /* Bind thread to a specific CPU */
    int target_cpu;
#if USE_THREAD_POOL == true
    static const int polling_cpus[] = POLLING_CPU_LIST;
    int cpu_index = POLLING_CPU_COUNT - 1 - (thread_id % POLLING_CPU_COUNT);
    target_cpu = polling_cpus[cpu_index];
#else
    static const int polling_cpus[] = POLLING_CPU_LIST;
    target_cpu = polling_cpus[POLLING_CPU_COUNT - 1];
#endif

    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(target_cpu, &cpu_set);
    if (sched_setaffinity(0, sizeof(cpu_set), &cpu_set) != 0) {
        printf("Failed to set polling thread %d affinity to CPU %d\n",
               thread_id, target_cpu);
    } else {
        printf("Polling thread %d bound to CPU %d\n", thread_id, target_cpu);
    }
    sched_yield();

    /* Set large budget to reduce preemption during busy-spin polling.
     * 100 ticks = 100ms at 1000Hz — long enough for low-latency polling,
     * but still allows other threads to eventually run. */
    usys_set_thread_budget(100);
    printf("Polling thread %d: set budget to 100 ticks\n", thread_id);

    int deq_count = 0;
    int poll_count = 0;

    while (1) {
#if ENABLE_SRV_TIMING
        struct timespec td0, td1, th0, th1;
        clock_gettime(CLOCK_MONOTONIC, &td0);
#endif
        struct dq_node *node = durable_dequeue(shm);
#if ENABLE_SRV_TIMING
        clock_gettime(CLOCK_MONOTONIC, &td1);
#endif

        // if (node == NULL) {
        //     poll_count++;
        //     if (poll_count % 5000000 == 0) {
        //         qptr_t h = atomic_load_explicit(&shm->queue.head, memory_order_relaxed);
        //         qptr_t t = atomic_load_explicit(&shm->queue.tail, memory_order_relaxed);
        //         qptr_t f = atomic_load_explicit(&shm->alloc.free_list, memory_order_relaxed);
        //         printf("[srv] idle %d deq=%d h=%d t=%d f=%d\n",
        //                poll_count / 5000000, deq_count, h, t, f);
        //     }
        //     __builtin_ia32_pause();
        //     continue;
        // }
        deq_count++;
        poll_count = 0;

#if ENABLE_SRV_TIMING
        clock_gettime(CLOCK_MONOTONIC, &th0);
#endif
        handle_polling_request(node);
#if ENABLE_SRV_TIMING
        clock_gettime(CLOCK_MONOTONIC, &th1);

        if (srv_perf_idx < SRV_PERF_MAX) {
            srv_t_deq[srv_perf_idx] = diff_ns(td0, td1);
            srv_t_handle[srv_perf_idx] = diff_ns(th0, th1);
            srv_perf_idx++;
        }
#endif

        FLUSH(node);
        atomic_store_explicit(&node->status, DQ_DONE,
                              memory_order_release);
        FLUSH(&node->status);
    }
}

/* ================================================================
 * Crash recovery: walk queue and mark DOING nodes as CRASH
 * ================================================================ */

void dq_recover_crash(struct polling_shm_region *shm)
{
    qptr_t cur = atomic_load_explicit(&shm->queue.head, memory_order_acquire);
    int recovered = 0;

    while (cur != QPTR_NULL) {
        struct dq_node *node = qptr_to_ptr(shm, cur);
        int status = atomic_load_explicit(&node->status, memory_order_acquire);

        if (status == DQ_DOING) {
            atomic_store_explicit(&node->status, DQ_CRASH,
                                  memory_order_release);
            FLUSH(&node->status);
            recovered++;
        }

        cur = atomic_load_explicit(&node->next, memory_order_acquire);
    }

    printf("[polling] dq_recover_crash: marked %d DOING nodes as CRASH\n",
           recovered);
}

/* ================================================================
 * Create polling threads
 * ================================================================ */

int create_polling_threads(u32 shm_id, pthread_t *tids, int num_threads)
{
    int ret;
    int created = 0;
    void *shm_addr;
    shm_addr = (void *)chcore_alloc_vaddr(POLLING_SHM_SIZE);
    ret = usys_mmap_shm(shm_id, shm_addr);
    if (ret < 0) {
        printf("Failed to mmap shm by id %d\n", shm_id);
        return 0;
    }
    printf("Polling Service Server: mmap shm by id %d\n", shm_id);
    struct polling_shm_region *shm = (struct polling_shm_region *)shm_addr;
    init_polling_shm_region(shm);

    struct polling_thread_arg *thread_args =
        (struct polling_thread_arg *)malloc(
                sizeof(struct polling_thread_arg) * num_threads);
    if (!thread_args) {
        printf("Failed to allocate memory for thread arguments\n");
        return 0;
    }

    for (int i = 0; i < num_threads; i++) {
        thread_args[i].shm = shm;
        thread_args[i].thread_id = i;
        ret = pthread_create(&tids[i], NULL, polling_reader_thread,
                             &thread_args[i]);
        if (ret != 0) {
            printf("Failed to create polling thread %d\n", i);
            break;
        }
        created++;
    }

    if (created == 0) {
        free(thread_args);
        return 0;
    }

    return created;
}

/* ================================================================
 * main
 * ================================================================ */

int main(int argc, char *argv[])
{
    my_id = usys_get_machine_id();
    assert(my_id >= 0);

#if USE_THREAD_POOL == true
    int num_threads = NUM_POLLING_THREADS;

    if (num_threads <= 0) {
        printf("NUM_POLLING_THREADS is %d, no polling threads will be created\n",
               num_threads);
        return 0;
    }

    pthread_t *tids = (pthread_t *)malloc(sizeof(pthread_t) * num_threads);
    if (!tids) {
        printf("Failed to allocate memory for thread IDs\n");
        return -1;
    }

    printf("Creating %d polling threads\n", num_threads);
    int created = create_polling_threads(my_id, tids, num_threads);
    if (created <= 0) {
        printf("No polling thread created\n");
        free(tids);
        return -1;
    }

    for (int i = 0; i < created; i++) {
        pthread_join(tids[i], NULL);
    }

    free(tids);
    printf("All polling threads exited\n");
#else
    pthread_t tid;
    if (create_polling_threads(my_id, &tid, 1) != 1) {
        printf("Failed to create polling thread\n");
        return -1;
    }
    pthread_join(tid, NULL);
    printf("Polling thread exited\n");
#endif
    return 0;
}
