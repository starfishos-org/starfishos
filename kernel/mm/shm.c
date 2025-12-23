#include <mm/vmspace.h>
#include <object/object.h>
#include <mm/mm.h>
#include <mm/shm.h>
#include <object/memory.h>
#include <dsm/dsm-single.h>
#include <common/lock.h>

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
                           SHM_DATA_SIZE,
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
        
        /* Initialize all message slots */
        for (int j = 0; j < MAX_MSG_COUNT; j++) {
            shm->msgs[j].magic = SHM_MSG_MAGIC(j);  /* Each slot has unique magic number */
            shm->msgs[j].type = SHM_MSG_TYPE_MAX;  /* Invalid type initially */
            shm->msgs[j].sender = 0xFFFFFFFF;
            shm->msgs[j].flag = SHM_MSG_FREE;
            lock_init(&shm->msgs[j].lock);
        }
        
        kinfo("[SHM] Initialized polling shm region for machine %d at %p, magic range: 0x%x-0x%x\n",
              i, shm, SHM_MSG_MAGIC(0), SHM_MSG_MAGIC(MAX_MSG_COUNT - 1));
    }
}

int sys_mmap_shm(u32 shm_id, void *addr)
{
    if (shm_id >= MAX_SHM_NUM) {
        kwarn("Invalid shm id: %d\n", shm_id);
        return -EINVAL;
    }
    u64 size = SHM_DATA_SIZE;
    int ret = 0;
    struct shm_data_t *shm_data = &dsm_meta->shm_data[shm_id];
    struct pmobject *pmo = shm_data->pmo;
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