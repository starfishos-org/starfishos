#pragma once

#include <chcore/type.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCI_CONTROL_LIST_DEVICES    (0)
#define PCI_CONTROL_OPEN_DEVICE     (1)

#define PCI_CONTROL_IVSHMEM_OPEN    (2)
#define PCI_CONTROL_IVSHMEM_CLOSE   (3)

struct pci_control_req {
    u64 req_type; // pcie control type
    char dev_ids[16];
    u64 arg_sz;
    u64 arg_ptr;
}; 

int usys_pcie_control(u64 req_buf);

#ifdef __cplusplus
}
#endif
