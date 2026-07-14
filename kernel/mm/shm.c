#include <mm/vmspace.h>
#include <object/object.h>
#include <object/thread.h>
#include <mm/mm.h>
#include <mm/shm.h>
#include <object/memory.h>
#include <dsm/dsm-single.h>
#include <common/lock.h>
#include <arch/sync.h>
#include <common/mem_sync.h>
#include <drivers/ivshmem.h>

extern int pmo_init(struct pmobject *pmo, pmo_type_t type, size_t len,
                    paddr_t paddr, mem_t mm_type, mem_t object_mem_type);

/*
 * CXLFS BAR addresses belong to each VM's local PCI layout.  Keep the PMO
 * descriptors kernel-local instead of publishing them through dsm_meta.
 */
static struct pmobject cxlfs_pmos[CLUSTER_MAX_MACHINE_NUM];
static bool cxlfs_pmos_ready;

static void cxlfs_shm_init(void)
{
    if (cxlfs_dev == NULL || cxlfs_dev->iosize <
                                     CLUSTER_MAX_MACHINE_NUM * CXLFS_SHM_SIZE) {
        kwarn("[SHM] CXLFS device is missing or too small\n");
        return;
    }

    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
        paddr_t slice_start = cxlfs_dev->iopa + i * CXLFS_SHM_SIZE;
        int ret = pmo_init(&cxlfs_pmos[i],
                           PMO_DEVICE,
                           CXLFS_SHM_SIZE,
                           slice_start,
                           __MT_SHARED__,
                           __MT_SHARED__);
        if (ret < 0) {
            kwarn("[SHM] Failed to register CXLFS region for machine %d\n", i);
            return;
        }
    }
    cxlfs_pmos_ready = true;
    kdebug("[SHM] Registered kernel-local CXLFS PMOs at BAR2 %llx\n",
           cxlfs_dev->iopa);
}

void shm_init(void)
{
    cxlfs_shm_init();

    /* Only init on first machine */
    if (CUR_MACHINE_ID != 0) {
        return;
    }

    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM && i < MAX_SHM_NUM; i++) {
        struct pmobject *new_pmo =
                obj_alloc(TYPE_PMO, sizeof(struct pmobject), __MT_SHARED__);
        int ret = pmo_init(new_pmo,
                           PMO_DATA,
                           POLLING_SHM_SIZE,
                           0,
                           __MT_SHARED__,
                           __MT_SHARED__);
        if (ret < 0) {
            printk("Failed to init shm pmo for machine %d\n", i);
            obj_free(new_pmo);
            continue;
        }
        void *shm_data = (void *)phys_to_virt(new_pmo->start);
        dsm_meta->shm_data[i].data = shm_data;
        dsm_meta->shm_data[i].pmo = new_pmo;

        /* Initialize durable queue for this machine */
        struct polling_shm_region *shm = (struct polling_shm_region *)shm_data;
        memset(shm, 0, POLLING_SHM_SIZE);

        s32 pool_off = DQ_POOL_OFFSET;
        s32 node_size = DQ_NODE_SIZE;
        s32 max_nodes = DQ_MAX_NODES;

        /* Allocator metadata */
        shm->alloc.node_size = node_size;
        shm->alloc.node_count = max_nodes;
        shm->alloc.pool_offset = pool_off;

        /* Sentinel node at node[0] */
        qptr_t sentinel_off = pool_off;
        struct dq_node *sentinel = qptr_to_ptr(shm, sentinel_off);
        sentinel->next = QPTR_NULL;
        sentinel->status = DQ_CONSUMED;

        /* Queue: head = tail = sentinel */
        shm->queue.head = sentinel_off;
        shm->queue.tail = sentinel_off;

        /* Build free list from node[1..max_nodes-1] */
        shm->alloc.free_list = QPTR_NULL;
        for (int j = max_nodes - 1; j >= 1; j--) {
            qptr_t off = pool_off + j * node_size;
            struct dq_node *node = qptr_to_ptr(shm, off);
            node->status = DQ_FREE;
            node->next = shm->alloc.free_list;
            shm->alloc.free_list = off;
        }

        kdebug("[SHM] Initialized durable queue for machine %d at %p"
               " (nodes=%d, node_size=%d)\n",
               i, shm, max_nodes, node_size);
    }

    /* Initialize p-log SHM regions (one per machine) */
    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM; i++) {
        int plog_id = PLOG_SHM_ID(i);
        struct pmobject *new_pmo =
                obj_alloc(TYPE_PMO, sizeof(struct pmobject), __MT_SHARED__);
        int ret = pmo_init(new_pmo,
                           PMO_DATA,
                           PLOG_SHM_SIZE,
                           0,
                           __MT_SHARED__,
                           __MT_SHARED__);
        if (ret < 0) {
            printk("Failed to init plog pmo for machine %d\n", i);
            obj_free(new_pmo);
            continue;
        }
        /*
         * A 4 MiB PMO_DATA uses the radix fallback and therefore has no
         * contiguous pmo->start.  User space initializes/validates the p-log
         * header after mapping; the kernel must not treat it as contiguous.
         */
        dsm_meta->shm_data[plog_id].data = NULL;
        dsm_meta->shm_data[plog_id].pmo = new_pmo;
        kdebug("[SHM] Initialized p-log region for machine %d (shm_id=%d)\n",
               i, plog_id);
    }

}

int sys_mmap_shm(u32 shm_id, void *addr)
{
    struct pmobject *pmo;
    u64 size;

    if (shm_id >= CXLFS_SHM_ID_BASE &&
        shm_id < CXLFS_SHM_ID_BASE + CLUSTER_MAX_MACHINE_NUM) {
        if (!cxlfs_pmos_ready) {
            kwarn("CXLFS SHM is not initialized\n");
            return -ENODEV;
        }
        size = CXLFS_SHM_SIZE;
        pmo = &cxlfs_pmos[shm_id - CXLFS_SHM_ID_BASE];
    } else if (shm_id >= MAX_SHM_NUM) {
        kwarn("Invalid shm id: %d\n", shm_id);
        return -EINVAL;
    } else {
        size = (shm_id >= PLOG_SHM_ID_BASE) ? PLOG_SHM_SIZE :
                                              POLLING_SHM_SIZE;
        pmo = dsm_meta->shm_data[shm_id].pmo;
    }
    if (pmo == NULL) {
        kwarn("SHM region %d is not initialized\n", shm_id);
        return -ENODEV;
    }
    int ret = 0;
    struct vmspace *vmspace =
            obj_get(current_cap_group, VMSPACE_OBJ_ID, TYPE_VMSPACE);
    if (vmspace == NULL) {
        printk("Failed to get vmspace\n");
        return -EFAULT;
    }

    ret = vmspace_map_range(
            vmspace, (vaddr_t)addr, size, VMR_READ | VMR_WRITE, pmo, NULL);
    if (ret < 0) {
        printk("Failed to map pmo\n");
        return -EFAULT;
    }

    return 0;
}

/* ================================================================
 * Kernel-side queue operations (used for TLB flush via polling)
 * Uses kernel atomic primitives instead of C11 _Atomic.
 * ================================================================ */

/*
 * Treiber stack pop (kernel side).
 */
static struct dq_node *alloc_node_try(struct polling_shm_region *shm)
{
    while (1) {
        s32 head = atomic_load_32(&shm->alloc.free_list);
        if (head == QPTR_NULL)
            return NULL;

        struct dq_node *node = qptr_to_ptr(shm, head);
        s32 next = atomic_load_32(&node->next);

        if (compare_and_swap_32(&shm->alloc.free_list, head, next) == head) {
            return node;
        }
    }
}

struct dq_node *dq_alloc_node(struct polling_shm_region *shm)
{
    while (1) {
        struct dq_node *node = alloc_node_try(shm);
        if (node != NULL)
            return node;
        /* Handle IPI while waiting to avoid deadlock */
        extern void handle_ipi(void);
        handle_ipi();
    }
}

/*
 * Lock-free enqueue (kernel side).
 */
void dq_enqueue(struct polling_shm_region *shm, struct dq_node *node,
                struct polling_request *req)
{
    qptr_t node_off = ptr_to_qptr(shm, node);

    /* Step 1: init node */
    memcpy(&node->req, req, sizeof(struct polling_request));
    atomic_store_32(&node->next, QPTR_NULL);
    atomic_store_32(&node->status, DQ_INIT);
    FLUSH(node);

    /* Step 2: link into queue */
    while (1) {
        s32 last = atomic_load_32(&shm->queue.tail);
        struct dq_node *last_node = qptr_to_ptr(shm, last);
        s32 next = atomic_load_32(&last_node->next);

        if (last != atomic_load_32(&shm->queue.tail))
            continue;

        if (next == QPTR_NULL) {
            if (compare_and_swap_32(&last_node->next, QPTR_NULL, node_off)
                == QPTR_NULL) {
                FLUSH(&last_node->next);
                compare_and_swap_32(&shm->queue.tail, last, node_off);
                return;
            }
        } else {
            FLUSH(&last_node->next);
            compare_and_swap_32(&shm->queue.tail, last, next);
        }
    }
}

void dq_wait_for_done(struct dq_node *node)
{
    extern void handle_ipi(void);
    while (atomic_load_32(&node->status) != DQ_DONE) {
        handle_ipi();
        CPU_PAUSE();
    }
}

/* ================================================================
 * Kernel-side thread durable queue operations
 * (for scheduler & notification waiting lists)
 * ================================================================ */

/* Helper functions for thread queue offset calculations */
void *thread_qptr_to_ptr(qptr_t off)
{
    if (off == QPTR_NULL)
        return NULL;
    return (char *)dsm_meta + off;
}

qptr_t thread_ptr_to_qptr(void *ptr)
{
    if (ptr == NULL)
        return QPTR_NULL;
    return (qptr_t)((char *)ptr - (char *)dsm_meta);
}

void thread_dq_pool_init(void)
{
    BUG_ON(!dsm_meta);
    struct thread_dq_pool *pool = &dsm_meta->thread_dq_pool;

    /* Initialize allocator metadata */
    pool->node_count = THREAD_DQ_POOL_SIZE;
    pool->free_list = QPTR_NULL;

    /* Build free list as Treiber stack (from last to first for proper order) */
    for (int j = THREAD_DQ_POOL_SIZE - 1; j >= 1; j--) {
        qptr_t off = thread_ptr_to_qptr(&pool->nodes[j]);
        struct thread_dq_node *node = &pool->nodes[j];
        node->status = DQ_FREE;
        node->next = pool->free_list;
        pool->free_list = off;
    }

    kdebug("[THREAD_DQ] Initialized thread durable queue pool"
           " (nodes=%d)\n", THREAD_DQ_POOL_SIZE);
}

static struct thread_dq_node *thread_dq_alloc_node_try(void)
{
    struct thread_dq_pool *pool = &dsm_meta->thread_dq_pool;
    while (1) {
        s32 head = pool->free_list;
        if (head == QPTR_NULL)
            return NULL;

        struct thread_dq_node *node = (struct thread_dq_node *)thread_qptr_to_ptr(head);
        s32 next = node->next;

        /* Try CAS pop */
        if (compare_and_swap_32((s32 *)&pool->free_list, head, next) == head) {
            return node;
        }
    }
}

static struct thread_dq_node *thread_dq_alloc_node(void)
{
    extern void handle_ipi(void);
    while (1) {
        struct thread_dq_node *node = thread_dq_alloc_node_try();
        if (node != NULL)
            return node;
        /* Handle IPI while waiting to avoid deadlock */
        handle_ipi();
    }
}

static void thread_dq_free_node(struct thread_dq_node *node)
{
    struct thread_dq_pool *pool = &dsm_meta->thread_dq_pool;
    qptr_t node_off = thread_ptr_to_qptr(node);

    /* Treiber stack push */
    while (1) {
        s32 head = pool->free_list;
        node->next = head;
        if (compare_and_swap_32((s32 *)&pool->free_list, head, node_off) == head) {
            return;
        }
    }
}

/*
 * Initialize a durable queue (set head = tail = new sentinel).
 */
int thread_dq_init(struct durable_queue *q)
{
    struct thread_dq_node *sentinel = thread_dq_alloc_node();
    if (!sentinel)
        return -ENOMEM;

    qptr_t sentinel_off = thread_ptr_to_qptr(sentinel);
    sentinel->next = QPTR_NULL;
    sentinel->status = DQ_CONSUMED;

    q->head = sentinel_off;
    q->tail = sentinel_off;
    lock_init(&q->queue_lock);

    return 0;
}

/*
 * Enqueue a thread to the durable queue (Michael-Scott enqueue).
 */
void thread_dq_enqueue(struct durable_queue *q, struct thread *thread)
{
    struct thread_dq_node *node = thread_dq_alloc_node();
    BUG_ON(!node);

    qptr_t node_off = thread_ptr_to_qptr(node);

    /* Store physical address of thread */
    node->thread_pa = virt_to_phys(thread);
    node->next = QPTR_NULL;
    node->status = DQ_INIT;

    /* For notification: store offset for cancellation */
    thread->notif_dq_node_off = node_off;

    /* Michael-Scott CAS enqueue */
    while (1) {
        qptr_t last = q->tail;
        struct thread_dq_node *last_node = (struct thread_dq_node *)thread_qptr_to_ptr(last);
        qptr_t next = last_node->next;

        if (last != q->tail)
            continue;

        if (next == QPTR_NULL) {
            if (compare_and_swap_32((s32 *)&last_node->next, QPTR_NULL, node_off) == QPTR_NULL) {
                compare_and_swap_32((s32 *)&q->tail, last, node_off);
                return;
            }
        } else {
            compare_and_swap_32((s32 *)&q->tail, last, next);
        }
    }
}

/*
 * Dequeue a thread from the durable queue (Michael-Scott dequeue, skip cancelled nodes).
 * Returns NULL if empty, otherwise returns the thread struct pointer.
 */
struct thread *thread_dq_dequeue(struct durable_queue *q)
{
    while (1) {
        qptr_t head = q->head;
        qptr_t tail = q->tail;
        struct thread_dq_node *head_node = (struct thread_dq_node *)thread_qptr_to_ptr(head);
        qptr_t next = head_node->next;

        if (head == q->head) {
            if (head == tail) {
                if (next == QPTR_NULL) {
                    return NULL;  /* Queue is empty */
                }
                /* Help advance tail */
                compare_and_swap_32((s32 *)&q->tail, tail, next);
            } else {
                struct thread_dq_node *next_node = (struct thread_dq_node *)thread_qptr_to_ptr(next);

                /* Skip cancelled nodes */
                if (next_node->status == DQ_CANCELLED) {
                    /* Advance head and free old sentinel */
                    if (compare_and_swap_32((s32 *)&q->head, head, next) == head) {
                        thread_dq_free_node(head_node);
                    }
                    continue;  /* Try again */
                }

                /* Try to advance head */
                if (compare_and_swap_32((s32 *)&q->head, head, next) == head) {
                    /* Extract thread pointer and free old sentinel */
                    u64 thread_pa = next_node->thread_pa;
                    thread_dq_free_node(head_node);

                    return (struct thread *)phys_to_virt(thread_pa);
                }
            }
        }
    }
}

/*
 * Mark a node as cancelled (for notification timeout/requeue).
 */
void thread_dq_cancel_node(qptr_t node_off)
{
    if (node_off == QPTR_NULL)
        return;

    struct thread_dq_node *node = (struct thread_dq_node *)thread_qptr_to_ptr(node_off);
    node->status = DQ_CANCELLED;
}
