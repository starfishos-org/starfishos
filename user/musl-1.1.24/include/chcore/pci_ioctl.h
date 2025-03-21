#pragma once

#include <chcore/type.h>

#include <sys/ioctl.h>
#include "vfio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_CONTROL_LIST_DEVICES    (0)
#define PCI_CONTROL_OPEN_DEVICE     (1)

struct pci_control_req {
    u64 req_type; // pcie control type
    char dev_ids[16];
    union {
        union {
            struct vfio_device_info device_info;
            struct vfio_region_info region_info;
            struct vfio_iommu_type1_dma_map dma_map;
        } _vfio_args;
    };
};

int usys_pcie_control(u64 req_buf);

#ifdef __cplusplus
}
#endif
