#pragma once

#include <common/types.h>
#include <common/list.h>
#include <drivers/resource.h>

#include "ioctl.h"

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

/* Userspace control syscalls */
int sys_pcie_control(u64 usr_req_buf);
