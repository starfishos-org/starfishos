#include <mm/vmspace.h>
#include <object/object.h>
#include <mm/mm.h>
#include <mm/shm.h>
#include <object/memory.h>
#include <dsm/dsm-single.h>
#include <common/lock.h>
#include <arch/sync.h>

extern int pmo_init(struct pmobject *pmo, pmo_type_t type, size_t len,
                    paddr_t paddr, mem_t mm_type, mem_t object_mem_type);

void shm_init(void)
{
    // only init for first machine
    if (CUR_MACHINE_ID != 0) {
        return;
    }
    /* Allocate shared memory for each machine */
    for (int i = 0; i < CLUSTER_MAX_MACHINE_NUM && i < MAX_SHM_NUM; i++) {
        struct pmobject *new_pmo =
                obj_alloc(TYPE_PMO, sizeof(struct pmobject), __MT_SHARED__);
        /* PMO_DATA will allocate memory internally, so we pass 0 for paddr */
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
        /* Get the virtual address of the allocated memory */
        void *shm_data = (void *)phys_to_virt(new_pmo->start);
        dsm_meta->shm_data[i].data = shm_data;
        dsm_meta->shm_data[i].pmo = new_pmo;

        /* Initialize durable queue for this machine */
        struct polling_shm_region *shm = (struct polling_shm_region *)shm_data;
        memset(shm, 0, sizeof(struct polling_shm_region));

        /* Node 0 is the initial sentinel */
        shm->nodes[0].next = NODE_NULL;
        shm->nodes[0].status = MSG_STATUS_DONE;

        /* Queue metadata */
        shm->meta.head = 0;
        shm->meta.tail = 0;
        shm->meta.pending_free = NODE_NULL;

        /* Build free list from nodes[1..MAX_NODE_COUNT-1] */
        for (int j = 1; j < (int)MAX_NODE_COUNT; j++) {
            shm->nodes[j].status = MSG_STATUS_FREE;
            shm->nodes[j].next = (j + 1 < (int)MAX_NODE_COUNT)
                                         ? j + 1 : NODE_NULL;
        }
        shm->meta.free_head = (MAX_NODE_COUNT > 1) ? 1 : NODE_NULL;

        kdebug("[SHM] Initialized durable queue for machine %d at %p\n",
              i, shm);
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
    printk("[SHM] sys_mmap_shm: shm_id=%d addr=%p size=%lu pmo=%p pmo->size=%lu pmo->type=%d\n",
           shm_id, addr, size, pmo, pmo ? pmo->size : 0, pmo ? pmo->type : -1);
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

/*
 * Treiber stack pop: allocate a node from the free list (kernel side).
 */
static struct shm_msg *alloc_node_retry(struct polling_shm_region *shm)
{
    while (1) {
        s32 head = atomic_load_32(&shm->meta.free_head);
        if (head == NODE_NULL)
            return NULL;

        struct shm_msg *node = &shm->nodes[head];
        s32 next = atomic_load_32(&node->next);

        if (compare_and_swap_32(&shm->meta.free_head, head, next) == head) {
            return node;
        }
    }
}

struct shm_msg *mpsc_alloc_msg(struct polling_shm_region *shm)
{
    while (1) {
        struct shm_msg *msg = alloc_node_retry(shm);
        if (msg != NULL) {
            return msg;
        }
        /* Handle IPI while waiting to avoid deadlock */
        extern void handle_ipi(void);
        handle_ipi();
    }
}

/*
 * Lock-free enqueue (Michael-Scott style, kernel side).
 */
void polling_enqueue(struct polling_shm_region *shm, struct shm_msg *node,
                     struct polling_request *req)
{
    /* Initialize node */
    memcpy(&node->req, req, sizeof(struct polling_request));
    atomic_store_32(&node->next, NODE_NULL);
    /* Release barrier: make req visible before status */
    atomic_store_32(&node->status, MSG_STATUS_INIT);

    s32 node_idx = (s32)(node - shm->nodes);

    /* Lock-free linking */
    while (1) {
        s32 last = atomic_load_32(&shm->meta.tail);
        struct shm_msg *last_node = &shm->nodes[last];
        s32 next = atomic_load_32(&last_node->next);

        /* Re-check tail consistency */
        if (last != atomic_load_32(&shm->meta.tail))
            continue;

        if (next == NODE_NULL) {
            /* Try to link our node to tail->next */
            if (compare_and_swap_32(&last_node->next, NODE_NULL, node_idx)
                == NODE_NULL) {
                /* Linked! Try to swing tail */
                compare_and_swap_32(&shm->meta.tail, last, node_idx);
                return;
            }
        } else {
            /* Help a crashed/slow enqueuer by advancing tail */
            compare_and_swap_32(&shm->meta.tail, last, next);
        }
    }
}

void polling_wait_for_response(struct shm_msg *msg)
{
    extern void handle_ipi(void);
    while (atomic_load_32(&msg->status) != MSG_STATUS_DONE) {
        /* Handle IPI while waiting to avoid deadlock */
        handle_ipi();
        CPU_PAUSE();
    }
}
