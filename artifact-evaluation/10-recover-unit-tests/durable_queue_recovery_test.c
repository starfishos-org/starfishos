#include "polling_req.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct crash_sim {
    _Alignas(64) unsigned char live[POLLING_SHM_SIZE];
    _Alignas(64) unsigned char durable[POLLING_SHM_SIZE];
};

static struct crash_sim *active_sim;

/* Production FLUSH/FLUSH_RANGE calls are redirected here by test_hooks.h. */
void dq_test_persist_range(const volatile void *addr, size_t len)
{
    const unsigned char *ptr = (const unsigned char *)addr;
    size_t offset;

    assert(active_sim != NULL);
    assert(ptr >= active_sim->live);
    offset = (size_t)(ptr - active_sim->live);
    assert(offset + len <= sizeof(active_sim->live));
    memcpy(active_sim->durable + offset, ptr, len);
}

static struct polling_shm_region *live_shm(struct crash_sim *sim)
{
    return (struct polling_shm_region *)sim->live;
}

static struct polling_shm_region *durable_shm(struct crash_sim *sim)
{
    return (struct polling_shm_region *)sim->durable;
}

static void sim_crash(struct crash_sim *sim)
{
    memcpy(sim->live, sim->durable, sizeof(sim->live));
}

static void sim_init(struct crash_sim *sim)
{
    struct polling_shm_region *shm;
    qptr_t sentinel_off = DQ_POOL_OFFSET;

    memset(sim, 0, sizeof(*sim));
    active_sim = sim;
    shm = live_shm(sim);
    atomic_init(&shm->queue.head, sentinel_off);
    atomic_init(&shm->queue.tail, sentinel_off);
    atomic_init(&shm->alloc.free_list, QPTR_NULL);
    shm->alloc.node_size = DQ_NODE_SIZE;
    shm->alloc.node_count = DQ_MAX_NODES;
    shm->alloc.pool_offset = DQ_POOL_OFFSET;

    struct dq_node *sentinel = qptr_to_ptr(shm, sentinel_off);
    atomic_init(&sentinel->next, QPTR_NULL);
    atomic_init(&sentinel->status, DQ_CONSUMED);

    for (int i = DQ_MAX_NODES - 1; i >= 1; --i) {
        qptr_t off = DQ_POOL_OFFSET + i * DQ_NODE_SIZE;
        struct dq_node *node = qptr_to_ptr(shm, off);
        atomic_init(&node->status, DQ_FREE);
        atomic_init(&node->next,
                    atomic_load_explicit(&shm->alloc.free_list,
                                         memory_order_relaxed));
        atomic_store_explicit(&shm->alloc.free_list, off,
                              memory_order_relaxed);
    }
    memcpy(sim->durable, sim->live, sizeof(sim->live));
}

static int valid_qptr(qptr_t off)
{
    return off >= DQ_POOL_OFFSET && off < (qptr_t)POLLING_SHM_SIZE
        && (off - DQ_POOL_OFFSET) % DQ_NODE_SIZE == 0;
}

static int queue_is_valid(struct polling_shm_region *shm)
{
    qptr_t cur = atomic_load_explicit(&shm->queue.head,
                                      memory_order_relaxed);
    qptr_t tail = atomic_load_explicit(&shm->queue.tail,
                                       memory_order_relaxed);

    if (!valid_qptr(cur) || !valid_qptr(tail))
        return 0;
    for (int steps = 0; steps < DQ_MAX_NODES; ++steps) {
        if (cur == tail)
            return 1;
        struct dq_node *node = qptr_to_ptr(shm, cur);
        cur = atomic_load_explicit(&node->next, memory_order_relaxed);
        if (!valid_qptr(cur))
            return 0;
    }
    return 0;
}

static struct dq_node *enqueue_empty(struct crash_sim *sim)
{
    struct polling_shm_region *shm = live_shm(sim);
    struct polling_request req = { .type = POLLING_REQ_EMPTY };
    struct dq_node *node = dq_alloc_node(shm);

    dq_enqueue(shm, node, &req);
    return node;
}

static int recover_doing(struct crash_sim *sim)
{
    struct polling_shm_region *shm = live_shm(sim);
    qptr_t cur = atomic_load_explicit(&shm->queue.head,
                                      memory_order_acquire);
    int recovered = 0;

    for (int steps = 0; cur != QPTR_NULL && steps < DQ_MAX_NODES; ++steps) {
        assert(valid_qptr(cur));
        struct dq_node *node = qptr_to_ptr(shm, cur);
        if (atomic_load_explicit(&node->status, memory_order_acquire)
            == DQ_DOING) {
            atomic_store_explicit(&node->status, DQ_ABORT,
                                  memory_order_release);
            FLUSH(&node->status);
            ++recovered;
        }
        cur = atomic_load_explicit(&node->next, memory_order_acquire);
    }
    return recovered;
}

static void complete_request(struct dq_node *node, int result,
                             int publish_done)
{
    node->resp.flush_tlb.reply_result = result;
    FLUSH(&node->resp.flush_tlb.reply_result);
    if (publish_done) {
        atomic_store_explicit(&node->status, DQ_DONE,
                              memory_order_release);
        FLUSH(&node->status);
    }
}

static void test_init_survives_and_completes(void)
{
    struct crash_sim sim;
    struct dq_node *node;

    sim_init(&sim);
    node = enqueue_empty(&sim);
    qptr_t node_off = ptr_to_qptr(live_shm(&sim), node);
    sim_crash(&sim);
    assert(queue_is_valid(live_shm(&sim)));
    node = qptr_to_ptr(live_shm(&sim), node_off);
    assert(atomic_load(&node->status) == DQ_INIT);

    assert(durable_dequeue(live_shm(&sim)) == node);
    complete_request(node, 0x1234, 1);
    sim_crash(&sim);
    node = qptr_to_ptr(live_shm(&sim), node_off);
    assert(queue_is_valid(live_shm(&sim)));
    assert(atomic_load(&node->status) == DQ_DONE);
    assert(node->resp.flush_tlb.reply_result == 0x1234);
}

static void test_doing_recovers_to_abort(void)
{
    struct crash_sim sim;
    struct dq_node *node;

    sim_init(&sim);
    node = enqueue_empty(&sim);
    qptr_t node_off = ptr_to_qptr(live_shm(&sim), node);
    assert(durable_dequeue(live_shm(&sim)) == node);
    sim_crash(&sim);
    node = qptr_to_ptr(live_shm(&sim), node_off);
    assert(queue_is_valid(live_shm(&sim)));
    assert(atomic_load(&node->status) == DQ_DOING);
    assert(recover_doing(&sim) == 1);
    sim_crash(&sim);
    node = qptr_to_ptr(live_shm(&sim), node_off);
    assert(atomic_load(&node->status) == DQ_ABORT);
}

static void test_response_without_done_is_aborted(void)
{
    struct crash_sim sim;
    struct dq_node *node;

    sim_init(&sim);
    node = enqueue_empty(&sim);
    qptr_t node_off = ptr_to_qptr(live_shm(&sim), node);
    assert(durable_dequeue(live_shm(&sim)) == node);
    complete_request(node, 0x5678, 0);
    sim_crash(&sim);
    node = qptr_to_ptr(live_shm(&sim), node_off);
    assert(node->resp.flush_tlb.reply_result == 0x5678);
    assert(atomic_load(&node->status) == DQ_DOING);
    assert(recover_doing(&sim) == 1);
    assert(atomic_load(&node->status) == DQ_ABORT);
}

static void test_head_survives_sentinel_reuse(void)
{
    struct crash_sim sim;
    struct polling_shm_region *shm;
    struct dq_node *first;
    qptr_t old_head;

    sim_init(&sim);
    shm = live_shm(&sim);
    old_head = atomic_load(&shm->queue.head);
    first = enqueue_empty(&sim);
    assert(durable_dequeue(shm) == first);
    complete_request(first, 1, 1);

    /* Reclaim and immediately reuse the old sentinel after head persisted. */
    dq_free_node(shm, qptr_to_ptr(shm, old_head));
    struct dq_node *second = enqueue_empty(&sim);
    assert(ptr_to_qptr(shm, second) == old_head);
    sim_crash(&sim);
    shm = live_shm(&sim);
    assert(queue_is_valid(shm));
    assert(atomic_load(&shm->queue.head) == ptr_to_qptr(shm, first));
    assert(atomic_load(&first->next) == ptr_to_qptr(shm, second));
}

static void test_tail_flush_is_required_before_head_flush(void)
{
    struct crash_sim sim;
    struct polling_shm_region *shm;
    qptr_t old_tail;

    sim_init(&sim);
    shm = live_shm(&sim);
    old_tail = atomic_load(&shm->queue.tail);
    (void)enqueue_empty(&sim);

    /* Fault injection: discard only the production tail persistence. */
    atomic_store(&durable_shm(&sim)->queue.tail, old_tail);
    assert(durable_dequeue(shm) != NULL); /* persists a newer head */
    sim_crash(&sim);
    assert(!queue_is_valid(live_shm(&sim)));
}

static void test_allocator_publication(void)
{
    struct crash_sim sim;
    struct polling_shm_region *shm;
    struct dq_node *node;
    qptr_t node_off;

    sim_init(&sim);
    shm = live_shm(&sim);
    node = dq_alloc_node(shm);
    node_off = ptr_to_qptr(shm, node);
    node->req.type = POLLING_PRINT_DEBUG_INFO; /* deliberately unflushed */
    sim_crash(&sim);
    shm = live_shm(&sim);
    assert(atomic_load(&shm->alloc.free_list) != node_off);

    node = qptr_to_ptr(shm, node_off);
    dq_free_node(shm, node);
    sim_crash(&sim);
    shm = live_shm(&sim);
    assert(atomic_load(&shm->alloc.free_list) == node_off);
    node = qptr_to_ptr(shm, node_off);
    assert(atomic_load(&node->status) == DQ_FREE);
}

struct test_case {
    const char *name;
    void (*run)(void);
};

int main(void)
{
    static const struct test_case cases[] = {
        { "INIT survives and completes", test_init_survives_and_completes },
        { "DOING recovers to ABORT", test_doing_recovers_to_abort },
        { "response without DONE aborts", test_response_without_done_is_aborted },
        { "head survives sentinel reuse", test_head_survives_sentinel_reuse },
        { "missing tail flush is detected", test_tail_flush_is_required_before_head_flush },
        { "allocator publication survives", test_allocator_publication },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            cases[i].run();
            _exit(0);
        }
        int status;
        assert(waitpid(pid, &status, 0) == pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL: %s\n", cases[i].name);
            return 1;
        }
        printf("PASS: %s\n", cases[i].name);
    }
    printf("durable_queue_recovery_test: PASS (%zu crash-recovery cases)\n",
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}
