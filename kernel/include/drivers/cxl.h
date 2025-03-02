#pragma once

#include <common/list.h>
#include <common/macro.h>
#include <drivers/pci.h>
#include <common/types.h>
#include <arch/mm/page_table.h>

#include "pci_cxl.h"

#define CXL_DEBUG

#define CXL_PREFIX "[cxl]"

#define cxl_info(fmt, ...)  printk(CXL_PREFIX " " fmt, ##__VA_ARGS__)
#define cxl_error(fmt, ...) printk(CXL_PREFIX " " fmt, ##__VA_ARGS__)
#ifdef CXL_DEBUG
#define cxl_debug(fmt, ...) printk(CXL_PREFIX " " fmt, ##__VA_ARGS__)
#else
#define cxl_debug(fmt, ...)
#endif

struct cxl_reg_map {
    bool valid;
    int id;
    unsigned long offset;
    unsigned long size;
};

struct cxl_component_reg_map {
    struct cxl_reg_map hdm_decoder;
    struct cxl_reg_map ras;
};

struct cxl_device_reg_map {
    struct cxl_reg_map status;
    struct cxl_reg_map mbox;
    struct cxl_reg_map memdev;
};

struct cxl_pmu_reg_map {
    struct cxl_reg_map pmu;
};

/**
 * struct cxl_register_map - DVSEC harvested register block mapping parameters
 * @dev: device for devm operations and logging
 * @base: virtual base of the register-block-BAR + @block_offset
 * @resource: physical resource base of the register block
 * @max_size: maximum mapping size to perform register search
 * @reg_type: see enum cxl_regloc_type
 * @component_map: cxl_reg_map for component registers
 * @device_map: cxl_reg_maps for device registers
 * @pmu_map: cxl_reg_maps for CXL Performance Monitoring Units
 */
struct cxl_register_map {
    struct pci_dev *pdev;
    void *base;
    resource_size_t resource;
    resource_size_t max_size;
    u8 reg_type;
    union {
        struct cxl_component_reg_map component_map;
        struct cxl_device_reg_map device_map;
        struct cxl_pmu_reg_map pmu_map;
    };
};

void cxl_probe_component_regs(struct pci_dev *dev, void *base,
                              struct cxl_component_reg_map *map);
void cxl_probe_device_regs(struct pci_dev *pdev, void *base,
                           struct cxl_device_reg_map *map);

enum cxl_regloc_type;

#define CXL_RESOURCE_NONE ((resource_size_t) - 1)
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
