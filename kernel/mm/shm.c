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

        /* Initialize polling shm region for this machine */
        struct polling_shm_region *shm = (struct polling_shm_region *)shm_data;
        memset(shm, 0, sizeof(struct polling_shm_region));

        kinfo("[SHM] Initialized polling shm region for machine %d at %p\n",
              i,
              shm);
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

struct shm_msg *mpsc_alloc_msg_retry(struct polling_shm_region *shm)
{
    s32 w = atomic_load_32(&shm->write_index);
    s32 r = atomic_load_32(&shm->read_index);

    if ((unsigned int)(w - r) >= MAX_MSG_COUNT - 1) {
        return NULL;
    }

    s32 idx = atomic_fetch_add_32(&shm->write_index, 1);
    struct shm_msg *msg = &shm->msgs[idx % MAX_MSG_COUNT];

    if (compare_and_swap_32(&msg->state, MSG_FREE, MSG_REQ_WRITING)
        != MSG_FREE) {
        return NULL;
    }

    return msg;
}

struct shm_msg *mpsc_alloc_msg(struct polling_shm_region *shm)
{
    while (1) {
        struct shm_msg *msg = mpsc_alloc_msg_retry(shm);
        if (msg != NULL) {
            return msg;
        }
    }
}

void polling_publish_request(struct shm_msg *msg, struct polling_request *req)
{
    memcpy(&msg->req, req, sizeof(struct polling_request));
    atomic_store_32(&msg->state, MSG_REQ_READY);
}

void polling_wait_for_response(struct shm_msg *msg)
{
    extern void handle_ipi(void);
    while (atomic_load_32(&msg->state) != MSG_RESP_READY) {
        /* Handle IPI while waiting to avoid deadlock */
        /* Similar to wait_finish_ipi_tx and migration_entry_wait */
        handle_ipi();
        CPU_PAUSE();
    }
}

void polling_free_msg(struct shm_msg *msg)
{
    atomic_store_32(&msg->state, MSG_FREE);
}
