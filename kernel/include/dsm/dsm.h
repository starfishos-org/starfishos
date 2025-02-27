// all structures should be packed
#pragma pack(1)

#include <arch/mmu.h>
#include <common/macro.h>
#include <arch/sync.h>
#include <common/size.h>
#include <common/types.h>
#include <common/mem_sync.h>
#include <arch/mm/page_table.h>

#define DSM_CONFIG_DEV_SZ      (8 * (u64)SIZE_1G)
#define DSM_CONFIG_MACHINE_NUM (2)

u32 CUR_MACHINE_ID;
// #define CUR_MACHINE_ID   (1001)
#define CUR_MACHINE_META (dsm_current_machine.meta)

#define DSM_MAX_MACHINE_NUM         (16)
#define DSM_PER_MACHINE_MAX_DEV_NUM (1)
#define DSM_MAX_DEV_NUM             (DSM_MAX_MACHINE_NUM * DSM_PER_MACHINE_MAX_DEV_NUM)

enum {
    MEMORY_DEVICE_TYPE_DRAM_SHARED,
    MEMORY_DEVICE_TYPE_DRAM_OWNED,
} dsm_memdev_type_t;

typedef struct {
    u8 type;
    u64 ownerid;
    u64 start;
    size_t size;
} dsm_mem_dev_t;

typedef struct {
    u64 machine_id;
    u64 dev_paddr;
} dsm_machine_dev_pair_t;

/**
 * @dsm_memdev_metadata_t
 *
 * This structure is used for comunications between machines
 *
 * v0:
 * every machine set OWNER_LOCAL_READY in its local memory.
 * leader (known by itself) wait for all device's OWNER_LOCAL_READY
 * have a global view of every devices, update this view
 * and set LEADER_GLOBAL_READY to enable each followers to config their
 * memory view.
 * Followers will update their memory config and set OWNER_READY
 * After everything is READY, run the system.
 */
typedef struct {
    volatile u64 max_ownerid;
    // local configs of this memory device,
    // should be consistent with memdev
    volatile u64 type;
    volatile u64 devid;
    volatile u64 ownerid;
    volatile size_t size;
    // global configuration
    enum {
        DSM_CONFIG_STATE_UNINITIALIZED = 0,
        DSM_CONFIG_STATE_LOCAL_CONFIG_READY = 1,
        DSM_CONFIG_STATE_CONFIG_READY,
        DSM_CONFIG_STATE_OWNER_READY,
    } dsm_config_state_type;
    volatile u64 state;
    // after configuration, should be consistent among all machines
    dsm_machine_dev_pair_t mapping[DSM_MAX_MACHINE_NUM];
} __attribute__((aligned(PAGE_SIZE))) dsm_memdev_metadata_t;

typedef struct {
    u64 machine_id;
    /* currently, only support 1 dev */
    u64 memdev_nums;
    dsm_mem_dev_t *own_memdevs[DSM_PER_MACHINE_MAX_DEV_NUM];
    /* metadata area for this machine to communicate with others */
    dsm_memdev_metadata_t *meta;
} dsm_machine_t;

/* current machine */
dsm_machine_t dsm_current_machine;

/* all dsm memdev */
int dsm_visible_memdev_num;
dsm_mem_dev_t dsm_visible_memdevs[DSM_MAX_DEV_NUM];

inline u64 dsm_get_memdev_start(dsm_mem_dev_t *dev)
{
    BUG_ON(!dev);
    return dev->start + sizeof(dsm_memdev_metadata_t);
}

inline u64 dsm_get_memdev_end(dsm_mem_dev_t *dev)
{
    BUG_ON(!dev);
    return dev->start + dev->size;
}

inline dsm_memdev_metadata_t *dsm_get_memdev_metadata(dsm_mem_dev_t *dev)
{
    return (dsm_memdev_metadata_t *)(phys_to_virt(dev->start));
}

/* dsm add memory devices */
dsm_mem_dev_t *dsm_add_visible_memdev(u64 start, size_t size, u8 type);
dsm_mem_dev_t *dsm_add_memdev_of_current_machine(u64 start, size_t size,
                                                 u8 type);
/* communications between machines */
int dsm_owner_init_metadata(dsm_memdev_metadata_t *meta);

/* config all visible devs */
void dsm_followers_init_metadata(void);

void dsm_devs_init(u64 start, size_t size, u8 type);
#pragma pack()
