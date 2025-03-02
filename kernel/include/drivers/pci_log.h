#pragma once

#include <common/types.h>
#include <common/list.h>
#include <drivers/resource.h>

#include "pci_regs.h"
#include "pci_names.h"

// #define PCI_DEBUG
#define PCI_IOCTL_DEBUG

#define PCI_PREFIX "[PCI]"

#define pci_info(fmt, ...)  printk(PCI_PREFIX " " fmt, ##__VA_ARGS__)
#define pci_error(fmt, ...) printk(PCI_PREFIX " " fmt, ##__VA_ARGS__)
#ifdef PCI_DEBUG
#define pci_debug(fmt, ...) printk(PCI_PREFIX " " fmt, ##__VA_ARGS__)
#else
#define pci_debug(fmt, ...)
#endif

#ifdef PCI_IOCTL_DEBUG
#define pci_ioctl_debug(fmt, ...) printk(PCI_PREFIX " " fmt, ##__VA_ARGS__)
#else
#define pci_ioctl_debug(fmt, ...)
#endif
