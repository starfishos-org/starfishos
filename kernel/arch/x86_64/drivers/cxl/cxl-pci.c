#include <common/list.h>
#include <drivers/pci.h>
#include <drivers/cxl-pci.h>
#include <mm/kmalloc.h>

#include "../pci/common.h"

extern struct lock pci_mmcfg_lock;
extern struct list_head pci_mmcfg_list;

void cxl_probe_devices()
{
        /* find CXL related devices in PCI memory config */

        /* probe registers for cxl devices */
}
