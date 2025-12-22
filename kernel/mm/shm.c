#include <mm/vmspace.h>
#include <object/object.h>
#include <mm/mm.h>
#include <mm/shm.h>
#include <object/memory.h>
#include <dsm/dsm-single.h>

extern int pmo_init(struct pmobject *pmo, pmo_type_t type, size_t len,
                    paddr_t paddr, mem_t mm_type, mem_t object_mem_type);

void shm_init(void)
{
    // only init for first machine
    if (CUR_MACHINE_ID != 0) {
        return;
    }
    for (int i = 0; i < MAX_SHM_NUM; i++) {
        void *shm_data = (void *)kmalloc(SHM_DATA_SIZE, __MT_SHARED__);
        if (shm_data == NULL) {
            printk("Failed to allocate shm data for shm id: %d\n", i);
            return;
        }
        struct pmobject *new_pmo =
                obj_alloc(TYPE_PMO, sizeof(struct pmobject), __MT_SHARED__);
        int ret = pmo_init(new_pmo,
                           PMO_SHM,
                           SHM_DATA_SIZE,
                           (paddr_t)shm_data,
                           __MT_SHARED__,
                           __MT_SHARED__);
        if (ret < 0) {
            printk("Failed to init shm pmo\n");
            obj_free(new_pmo);
            return;
        }
        dsm_meta->shm_data[i].data = shm_data;
        dsm_meta->shm_data[i].pmo = new_pmo;
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