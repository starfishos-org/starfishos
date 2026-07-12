#include "polling_req.h"
#include "polling_config.h"

#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ================================================================
 * Node Allocator — Treiber stack (lock-free)
 * ================================================================ */

/*
 * Pop a node from the free list.
 * Returns NULL if pool is exhausted.
 */
static struct dq_node *alloc_node_try(struct polling_shm_region *shm)
{
    while (1) {
        qptr_t head = atomic_load_explicit(&shm->alloc.free_list,
                                           memory_order_acquire);
        if (head == QPTR_NULL)
            return NULL;

        struct dq_node *node = qptr_to_ptr(shm, head);
        qptr_t next = atomic_load_explicit(&node->next,
                                           memory_order_relaxed);

        if (atomic_compare_exchange_weak_explicit(
                    &shm->alloc.free_list, &head, next,
                    memory_order_release, memory_order_relaxed)) {
            return node;
        }
    }
}

/*
 * Allocate a node, spinning until one is available.
 */
struct dq_node *dq_alloc_node(struct polling_shm_region *shm)
{
    int spins = 0;
    while (1) {
        struct dq_node *node = alloc_node_try(shm);
        if (node != NULL)
            return node;
        if (++spins % 10000000 == 0) {
            qptr_t f = atomic_load_explicit(&shm->alloc.free_list, memory_order_relaxed);
            printf("[alloc_stuck] spins=%d free_list=%d\n", spins, f);
        }
        sched_yield();
    }
}

/*
 * Push a node back to the free list (Treiber stack push).
 */
void dq_free_node(struct polling_shm_region *shm, struct dq_node *node)
{
    qptr_t node_off = ptr_to_qptr(shm, node);
    atomic_store_explicit(&node->status, DQ_FREE, memory_order_relaxed);
    while (1) {
        qptr_t head = atomic_load_explicit(&shm->alloc.free_list,
                                           memory_order_acquire);
        atomic_store_explicit(&node->next, head, memory_order_relaxed);
        if (atomic_compare_exchange_weak_explicit(
                    &shm->alloc.free_list, &head, node_off,
                    memory_order_release, memory_order_relaxed)) {
            return;
        }
    }
}

/* ================================================================
 * Enqueue — Algorithm from docs/durable-queue.md
 *
 *   node <- new Node(INIT, payload); FLUSH(node)
 *   while true:
 *     last <- tail; next <- last->next
 *     if last == tail:
 *       if next == NULL:
 *         if CAS(&last->next, next, node):
 *           FLUSH(&last->next)
 *           CAS(&tail, last, node)
 *           return node
 *       else:
 *         FLUSH(&last->next)          // help crashed enqueuer
 *         CAS(&tail, last, next)
 * ================================================================ */

void dq_enqueue(struct polling_shm_region *shm, struct dq_node *node,
                struct polling_request *req)
{
    qptr_t node_off = ptr_to_qptr(shm, node);

    /* Step 1: init node with payload */
    memcpy(&node->req, req, sizeof(struct polling_request));
    atomic_store_explicit(&node->next, QPTR_NULL, memory_order_relaxed);
    atomic_store_explicit(&node->status, DQ_INIT, memory_order_release);
    FLUSH(node);

    /* Step 2: link into queue */
    int enq_spins = 0;
    while (1) {
        qptr_t last = atomic_load_explicit(&shm->queue.tail,
                                           memory_order_acquire);
        struct dq_node *last_node = qptr_to_ptr(shm, last);
        qptr_t next = atomic_load_explicit(&last_node->next,
                                           memory_order_acquire);

        /* Re-check tail consistency */
        if (last != atomic_load_explicit(&shm->queue.tail,
                                         memory_order_acquire))
            continue;

        if (next == QPTR_NULL) {
            /* Try to link our node to tail->next */
            qptr_t expected = QPTR_NULL;
            if (atomic_compare_exchange_strong_explicit(
                        &last_node->next, &expected, node_off,
                        memory_order_release, memory_order_relaxed)) {
                FLUSH(&last_node->next);
                /* Swing tail to our node */
                qptr_t exp_last = last;
                atomic_compare_exchange_strong_explicit(
                        &shm->queue.tail, &exp_last, node_off,
                        memory_order_release, memory_order_relaxed);
                return;
            }
        } else {
            /* Help a crashed/slow enqueuer by advancing tail */
            FLUSH(&last_node->next);
            qptr_t exp_last = last;
            atomic_compare_exchange_strong_explicit(
                    &shm->queue.tail, &exp_last, next,
                    memory_order_release, memory_order_relaxed);
        }

        if (++enq_spins % 10000000 == 0) {
            qptr_t h = atomic_load_explicit(&shm->queue.head, memory_order_relaxed);
            qptr_t t = atomic_load_explicit(&shm->queue.tail, memory_order_relaxed);
            printf("[enq_stuck] spins=%d last=%d next=%d h=%d t=%d node_off=%d\n",
                   enq_spins, last, next, h, t, node_off);
        }
    }
}

/* ================================================================
 * Wait — producer spins on status == DQ_DONE
 * ================================================================ */

void dq_wait_for_done(struct dq_node *node)
{
    int spins = 0;
    while (1) {
        int status = atomic_load_explicit(&node->status, memory_order_acquire);
        if (status == DQ_DONE)
            return;
        if (status == DQ_CRASH) {
            printf("[dq_wait] node crashed, server may have died\n");
            return;
        }
        if (++spins % 10000000 == 0) {
            printf("[wait_stuck] spins=%d status=%d\n", spins, status);
        }
    }
}

/* Returns 0 if DONE, -1 if CRASH */
int dq_wait_for_done_or_crash(struct dq_node *node)
{
    int spins = 0;
    while (1) {
        int status = atomic_load_explicit(&node->status, memory_order_acquire);
        if (status == DQ_DONE)
            return 0;
        if (status == DQ_CRASH)
            return -1;
        if (++spins % 10000000 == 0) {
            printf("[wait_stuck] spins=%d status=%d\n", spins, status);
        }
    }
}

/* ================================================================
 * Server-side dequeue — shared by polling_server.c and tmpfs_polling.c
 *
 * Deferred-free ring: after a node is dequeued it becomes the new queue
 * sentinel and cannot be freed immediately (the producer may still be
 * reading the response).  We delay by DEFER_DEPTH dequeue cycles; the
 * slot being evicted must be DQ_CONSUMED before we call dq_free_node.
 * ================================================================ */

#define DEFER_DEPTH 16
static qptr_t defer_ring[DEFER_DEPTH];
static int defer_idx = 0;
static int defer_inited = 0;

static void defer_free(struct polling_shm_region *shm, qptr_t old_sentinel)
{
    if (!defer_inited) {
        for (int i = 0; i < DEFER_DEPTH; i++)
            defer_ring[i] = QPTR_NULL;
        defer_inited = 1;
    }

    qptr_t to_free = defer_ring[defer_idx];
    if (to_free != QPTR_NULL) {
        struct dq_node *node = qptr_to_ptr(shm, to_free);
        while (atomic_load_explicit(&node->status, memory_order_acquire)
               != DQ_CONSUMED) {
            __builtin_ia32_pause();
        }
        dq_free_node(shm, node);
    }

    defer_ring[defer_idx] = old_sentinel;
    defer_idx = (defer_idx + 1) % DEFER_DEPTH;
}

struct dq_node *durable_dequeue(struct polling_shm_region *shm)
{
    while (1) {
        qptr_t first = atomic_load_explicit(&shm->queue.head,
                                            memory_order_acquire);
        qptr_t last = atomic_load_explicit(&shm->queue.tail,
                                           memory_order_acquire);
        struct dq_node *first_node = qptr_to_ptr(shm, first);
        qptr_t next = atomic_load_explicit(&first_node->next,
                                           memory_order_acquire);

        if (first != atomic_load_explicit(&shm->queue.head,
                                          memory_order_acquire))
            continue;

        if (first == last) {
            if (next == QPTR_NULL)
                return NULL;
            FLUSH(&first_node->next);
            qptr_t exp = last;
            atomic_compare_exchange_strong_explicit(
                    &shm->queue.tail, &exp, next,
                    memory_order_release, memory_order_relaxed);
        } else {
            struct dq_node *n = qptr_to_ptr(shm, next);

            int expected = DQ_INIT;
            if (atomic_compare_exchange_strong_explicit(
                        &n->status, &expected, DQ_DOING,
                        memory_order_acquire, memory_order_relaxed)) {
                FLUSH(&n->status);

                qptr_t exp_first = first;
                atomic_compare_exchange_strong_explicit(
                        &shm->queue.head, &exp_first, next,
                        memory_order_release, memory_order_relaxed);

                defer_free(shm, first);
                return n;
            }

            qptr_t exp_first = first;
            atomic_compare_exchange_strong_explicit(
                    &shm->queue.head, &exp_first, next,
                    memory_order_release, memory_order_relaxed);
        }
    }
}

/* ================================================================
 * Debug
 * ================================================================ */

void debug_print_shm_region(struct polling_shm_region *shm)
{
    qptr_t h = atomic_load_explicit(&shm->queue.head, memory_order_relaxed);
    qptr_t t = atomic_load_explicit(&shm->queue.tail, memory_order_relaxed);
    qptr_t f = atomic_load_explicit(&shm->alloc.free_list, memory_order_relaxed);
    printf("durable_queue: head=%d, tail=%d, free_list=%d, nodes=%d\n",
           h, t, f, shm->alloc.node_count);
}

void debug_print_mpsc_alloc_msg_retry_time(void)
{
    /* placeholder */
}

/* ================================================================
 * High-level FS operations (producer side)
 *
 * Pattern: alloc -> enqueue -> wait DONE -> read resp -> mark CONSUMED
 * ================================================================ */

int polling_fs_open(struct polling_shm_region *shm, const char *path, int flags,
                    int mode)
{
    struct polling_request req = {
            .type = POLLING_FS_REQ_OPEN,
            .open = { .flags = flags, .mode = mode },
    };
    strncpy(req.open.path, path, FS_REQ_PATH_BUF_LEN);

    struct dq_node *node = dq_alloc_node(shm);
    dq_enqueue(shm, node, &req);
    dq_wait_for_done(node);

    int fd = node->resp.open.fd;
    atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);
    return fd;
}

ssize_t polling_fs_read(struct polling_shm_region *shm, int fd, void *buf,
                        size_t count)
{
    size_t left = count;
    char *p = buf;
    ssize_t total = 0;

    while (left > 0) {
        size_t chunk = left < POLLING_FS_READ_BUF_SIZE
                             ? left : POLLING_FS_READ_BUF_SIZE;

        struct polling_request req = {
                .type = POLLING_FS_REQ_READ,
                .read = { .fd = fd, .count = chunk },
        };

        struct dq_node *node = dq_alloc_node(shm);
        dq_enqueue(shm, node, &req);
        dq_wait_for_done(node);

        ssize_t n = node->resp.read.count;
        memcpy(p, node->resp.read.buf, n);
        atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);

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
        size_t chunk = left < POLLING_FS_WRITE_BUF_SIZE
                             ? left : POLLING_FS_WRITE_BUF_SIZE;

        struct polling_request req = {
                .type = POLLING_FS_REQ_WRITE,
                .write = { .fd = fd, .count = chunk },
        };
        memcpy(req.write.buf, p, chunk);

        struct dq_node *node = dq_alloc_node(shm);
        dq_enqueue(shm, node, &req);
        dq_wait_for_done(node);

        ssize_t n = node->resp.write.count;
        atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);

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
            .close = { .fd = fd },
    };

    struct dq_node *node = dq_alloc_node(shm);
    dq_enqueue(shm, node, &req);
    dq_wait_for_done(node);

    int ret = node->resp.close.ret;
    atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);
    return ret;
}

void polling_fs_empty(struct polling_shm_region *shm)
{
    struct polling_request req = {
            .type = POLLING_REQ_EMPTY,
    };
    struct dq_node *node = dq_alloc_node(shm);
    dq_enqueue(shm, node, &req);
    dq_wait_for_done(node);
    atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);
}

void polling_print_debug_info(struct polling_shm_region *shm)
{
    struct polling_request req = {
            .type = POLLING_PRINT_DEBUG_INFO,
    };
    struct dq_node *node = dq_alloc_node(shm);
    dq_enqueue(shm, node, &req);
    dq_wait_for_done(node);
    atomic_store_explicit(&node->status, DQ_CONSUMED, memory_order_release);
}
