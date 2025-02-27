#pragma once

#include <sys/types.h>
#include <string.h>

#include "fd.h"

#define IS_DEVFS(path) (strncmp(path, "/dev/", 5) == 0)
#define IS_DEV_VIRTIO_FS(path) (strncmp(path, "/dev/virtio", 11) == 0)

extern struct fd_ops virtio_file_ops;

// Following structures should be defined in kernel
// path: kernel/include/drivers/pci.h
enum pci_control_type {
    PCI_CONTROL_LIST_DEVICES = 0,
    PCI_CONTROL_GET_INFO = 1,
    PCI_CONTROL_MAP_REGION = 2,
};

struct pci_dev_req {
    u8 req_type; // pcie control type
    char dev_path[64]; // e.g. "/dev/pcie0"
    char dev_ids[11]; // in format of "0000:00:00.0"
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

int chcore_virtio_file_ioctl(int fd, unsigned long request, void *arg);
int chcore_open_dev(char *path);
