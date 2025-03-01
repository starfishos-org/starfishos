#pragma once

#include <common/types.h>
#include <common/list.h>
#include <drivers/resource.h>

enum pci_control_type {
    PCI_CONTROL_LIST_DEVICES = 0,
    PCI_CONTROL_GET_INFO = 1,
    PCI_CONTROL_MAP_REGION = 2,
};

static const char *const pci_control_type_str[] = {
    "LIST DEVICES",
    "GET_INFO",
    "MAP_REGION",
};

struct pci_dev_req {
    u8 req_type; // pcie control type
    char dev_path[64]; // e.g. "/dev/pcie0"
    char dev_ids[13]; // in format of "0000:00:00.0"
    // align to 16 bytes
    char padding[3];
    // mmio region
    u64 mmio_vaddr;
    u64 mmio_size;
    // io region
    u64 io_vaddr;
    u64 io_size;
    // irq region
    u64 irq_vaddr;
    u64 irq_size;
    // return info
    char ret[128];
};

/* Userspace control syscalls */
int sys_pcie_control(u64 usr_req_buf);
