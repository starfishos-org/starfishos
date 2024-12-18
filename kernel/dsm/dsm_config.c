#include <dsm/dsm.h>
#include <common/size.h>

static dsm_mem_dev_t *dsm_find_in_visible_memdevs(u64 start, size_t size)
{
        dsm_mem_dev_t *dev = NULL;
        for (int idx = 0; idx < dsm_visible_memdev_num; idx++) {
                dev = &(dsm_visible_memdevs[idx]);
                if (dev->start == start && dev->size == size) {
                        return dev;
                }
        }
        return NULL;
}
#if 0
static void __hack_set_offset_of_dimm_dev(dsm_mem_dev_t *dev)
{
        if (dev->start >= 0x180000000) {
                dev->start += SIZE_2G;
                dev->size -= SIZE_2G;
        }
}
#endif

/**
 * dsm_add_visible_memdev:
 *
 * add a memdev belongs to current machine
 * @start, @size, @type: args of this memdev
 */
dsm_mem_dev_t *dsm_add_visible_memdev(u64 start, size_t size, u8 type)
{
        if (size < SIZE_2G)
                return NULL;
        u64 idx = 0;
        /* get idx of memory dev machine */
        idx = atomic_fetch_add_64(&(dsm_visible_memdev_num), 1);
        BUG_ON(idx > (u64)DSM_MAX_DEV_NUM);
        /* setup args */
        dsm_mem_dev_t *dev = &(dsm_visible_memdevs[idx]);
        dev->start = start;
        dev->size = size;
        dev->type = type;
        // __hack_set_offset_of_dimm_dev(dev);
        kinfo("[DSM] init visible memdev %llx - %llx\n", start, start + size);
        return dev;
}

static inline void __set_machine_metadata(dsm_memdev_metadata_t *meta)
{
        dsm_current_machine.meta = meta;
}
/**
 * add_memdev_of_current_machine:
 *
 * add a memdev belongs to current machine
 * @start, @size, @type: args of this memdev
 */
extern inline dsm_memdev_metadata_t *dsm_get_memdev_metadata(dsm_mem_dev_t *dev);
dsm_mem_dev_t *dsm_add_memdev_of_current_machine(u64 start, size_t size,
                                                 u8 type)
{
        u64 idx = 0;
        /* get idx of memory dev machine */
        idx = atomic_fetch_add_64(&(dsm_current_machine.memdev_nums), 1);
        BUG_ON(idx > DSM_PER_MACHINE_MAX_DEV_NUM);

        /* setup args */
        dsm_mem_dev_t *dev = dsm_find_in_visible_memdevs(start, size);
        if (!dev) {
                dev = dsm_add_visible_memdev(start, size, type);
                BUG_ON(!dev);
        }

        dev->ownerid = CUR_MACHINE_ID;
        dsm_current_machine.own_memdevs[idx] = dev;
        kinfo("current machine dev start=%llx\n", dev->start);

        /* Use the first own memdev as communication */
        __set_machine_metadata(dsm_get_memdev_metadata(dev));

        return dev;
}

/* DSM Communication */
int dsm_owner_init_metadata(dsm_memdev_metadata_t *meta)
{
        int r = 0;

        CUR_MACHINE_META->devid = CUR_MACHINE_ID;
        FLUSH(&CUR_MACHINE_META->devid);
        CUR_MACHINE_META->state = DSM_CONFIG_STATE_LOCAL_CONFIG_READY;
        FLUSH(&CUR_MACHINE_META->state);
        FENCE;
        return r;
}

void dsm_followers_init_metadata(void)
{
        while (1) {
                int idx = 0;
                for (idx = 0; idx < dsm_visible_memdev_num; idx++) {
                        dsm_mem_dev_t *dev = &dsm_visible_memdevs[idx];
                        dsm_memdev_metadata_t *meta =
                                dsm_get_memdev_metadata(dev);
                        // if (meta->ownerid == CUR_MACHINE_ID)
                        //      continue;
                        kinfo("[DSM] [BEFPRE] meta=%p, state=%d ownerid=%d\n",
                              meta,
                              meta->state,
                              meta->ownerid);
                        while (meta->state == DSM_CONFIG_STATE_UNINITIALIZED)
                                for (int loop = 0; loop < 1000000000; loop++)
                                        ; // start loop again;

                        //                   while (meta->state ==
                        //                   DSM_CONFIG_STATE_UNINITIALIZED) {
                        //               }
                        kinfo("[DSM] [AFTER] meta=%p, state=%d ownerid=%d\n",
                              meta,
                              meta->state,
                              meta->ownerid);
                }
                if (idx == dsm_visible_memdev_num)
                        break;
        }
        kinfo("All device is ready");
        BUG_ON(1);
}

/* In current emulation:
 * @start: start of a shared device,
 * @end: end of a shared device
 *
 * We will part the device into MACHINE_NUM part and link to each device
 * */

void dsm_devs_init(u64 start, size_t size, u8 type)
{
        u32 ownerid;
        u64 dev_start;
        size_t dev_size;

        ownerid = atomic_fetch_add_32((u32 *)phys_to_virt(start), 1);
        CUR_MACHINE_ID = ownerid;
        kinfo("ownerid = %u", ownerid);
        for (int idx = 0; idx < (int)DSM_CONFIG_MACHINE_NUM; idx++) {
                dev_start = (u64)(start + idx * DSM_CONFIG_DEV_SZ);
                dev_size = (size_t)DSM_CONFIG_DEV_SZ;
                dsm_add_visible_memdev(dev_start, dev_size, type);
                if (idx == ownerid) {
                        dsm_add_memdev_of_current_machine(
                                dev_start, dev_size, type);
                }
        }
}
