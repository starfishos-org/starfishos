#include "common/list.h"
#include "common/macro.h"
#include "drivers/pci.h"
#include <common/types.h>
#include <arch/mm/page_table.h>

struct cxl_dev_state {
        struct pci_dev *pci_dev;
        int cxl_dvsec;
};
struct cxl_memdev_state {
        struct cxl_dev_state *cxlds;
};

/* HACK: a fast path */
int arch_pci_find_cxl_devices(struct list_head *list);

struct cxl_hdm {
        void *hdm_base;
        u32 decoder_count;
        u32 target_count;
        u32 interleave_mask;
};

struct cxl_hdm cxl_hdm;

struct cxl_chbs_context {
        u64 uid;
        u64 base;
        u32 cxl_version;
};

#define MAX_CXL_CHBS_NUM (16)

struct cxl_chbs_context cxl_chbs_ctxs[MAX_CXL_CHBS_NUM];
int cxl_chbs_ctxs_num;

// CXL TYE3
struct cxl_mem_dev {
        u64 devid;
        u64 start;
        u64 size;
        struct pci_dev *device;

} __attribute__((packed));

typedef struct cxl_mem_dev cxl_mem_dev_t;

struct cxl_mem_dev cxl_mem_devs[MAX_CXL_CHBS_NUM];
int cxl_mem_dev_num;

inline u64 cxl_get_memdev_start(cxl_mem_dev_t *dev)
{
        BUG_ON(!dev);
        return dev->start;
}

inline u64 cxl_get_memdev_end(cxl_mem_dev_t *dev)
{
        BUG_ON(!dev);
        return dev->start + dev->size;
}

void cxl_setup_devices();
