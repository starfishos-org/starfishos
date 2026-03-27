#include <mm/vmspace.h>
#include <object/object.h>
#include <mm/mm.h>
#include <mm/shm.h>
#include <object/memory.h>
#include <dsm/dsm-single.h>
#include <common/lock.h>
#include <arch/sync.h>
#include <common/mem_sync.h>

extern int pmo_init(struct pmobject *pmo, pmo_type_t type, size_t len,
                    paddr_t paddr, mem_t mm_type, mem_t object_mem_type);

void shm_init(void)
{
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
        sentinel->status = DQ_DONE;

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
}

int sys_mmap_shm(u32 shm_id, void *addr)
{
    if (shm_id >= MAX_SHM_NUM) {
        kwarn("Invalid shm id: %d\n", shm_id);
        return -EINVAL;
    }
    u64 size = POLLING_SHM_SIZE;
    int ret = 0;
    struct pmobject *pmo = dsm_meta->shm_data[shm_id].pmo;
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
