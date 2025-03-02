#pragma once

#include <common/types.h>
#include <common/list.h>
#include <drivers/resource.h>

#include "ioctl.h"
#include "vfio.h"

#define PCI_CONTROL_LIST_DEVICES    (0)
#define PCI_CONTROL_OPEN_DEVICE     (1)

struct pci_control_req {
    u32 req_type; // pcie control type
    char dev_ids[16];
    union {
        union {
            struct vfio_device_info device_info;
            struct vfio_region_info region_info;
            struct vfio_iommu_type1_dma_map dma_map;
        } _vfio_args;
    };
}; 

/* Userspace control syscalls */
int sys_pcie_control(u64 usr_req_buf);
