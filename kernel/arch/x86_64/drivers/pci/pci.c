#include <common/lock.h>
#include <common/list.h>
#include <drivers/pci.h>
#include <drivers/pci-special.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>

#include "common.h"
#include "../acpi/acpi.h"
#include <drivers/cxl-pci.h>

extern struct lock pci_mmcfg_lock;
extern struct list_head pci_mmcfg_list;

struct pci_bus *pci_root_bus;

static void add_pci_bus(struct pci_bus *new_bus)
{
        if (!pci_root_bus) {
                pci_root_bus = new_bus;
        } else {
                list_add(&new_bus->node, &pci_root_bus->children);
                new_bus->parent = pci_root_bus;
        }
        init_list_head(&new_bus->children);
}

void pci_devices_traverse(struct pci_bus *bus, pci_bus_traverse_fn func)
{
        struct pci_dev *pdev;
        for_each_in_list (pdev, struct pci_dev, bus_list, &bus->devices) {
                func(pdev);
        }
}

void pci_buses_traverse(struct pci_bus *parent, pci_bus_traverse_fn func)
{
        struct pci_bus *child;
        pci_devices_traverse(parent, func);

        for_each_in_list (child, struct pci_bus, node, &parent->children) {
                pci_buses_traverse(child, func);
        }
}

void pci_buses_traverse_all(pci_bus_traverse_fn func)
{
        pci_buses_traverse(pci_root_bus, func);
}

void arch_pci_mmcfg_init()
{
        lock_init(&pci_mmcfg_lock);
        init_list_head(&pci_mmcfg_list);

        // acpi table parse pci mmconfig
        /* find and parse MCFG */
        acpi_parse_pci_info();
}

static void pci_get_devid_and_vendorid(struct pci_mmcfg_region *region,
                                       int bus_n, unsigned int devfn,
                                       u16 *dev_id, u16 *vendor_id)
{
        void *cfg =
                region->virt + (PCI_MMCFG_BUS_OFFSET(bus_n) | (devfn << 12));

        pci_config_readw(cfg, PCI_DEVICE_ID, dev_id);
        pci_config_readw(cfg, PCI_VENDOR_ID, vendor_id);
}

static void arch_pci_mmcfg_probe_devices(struct pci_mmcfg_region *region)
{
        int start_bus_n = region->start_bus, end_bus_n = region->end_bus;
        int bus_n;
        unsigned int devfn;
        u16 dev_id, vendor_id;

        /*  parse each bus on the memory region */
        for (bus_n = start_bus_n; bus_n < end_bus_n; bus_n++) {
                struct pci_bus *bus;
                pci_get_devid_and_vendorid(
                        region, bus_n, 0, &dev_id, &vendor_id);
                if (vendor_id != 0xffff) {
                        bus = dram_kzalloc(sizeof(*bus));
                        bus->domain = region->segment;
                        bus->number = (char)bus_n;
                        init_list_head(&bus->devices);
                        add_pci_bus(bus);
                } else {
                        continue;
                }
                for (devfn = 0; devfn < 255; devfn++) {
                        pci_get_devid_and_vendorid(
                                region, bus_n, devfn, &dev_id, &vendor_id);
                        /* set up device */
                        if (vendor_id != 0xffff) {
                                pci_debug(
                                        "[probe device] BUS: %d DEVID: %04x VENDORID: %04x \n",
                                        bus_n,
                                        dev_id,
                                        vendor_id);
                                struct pci_dev *dev =
                                        dram_kzalloc(sizeof(*dev));

                                dev->device = dev_id;
                                dev->vendor = vendor_id;
                                dev->devfn = devfn;
                                dev->bus = bus;
                                list_add(&dev->bus_list, &bus->devices);
                                pci_setup_device(dev);
                        }
                }
        }
}

void arch_pci_probe_devices()
{
        struct pci_mmcfg_region *region;

        for_each_in_list (
                region, struct pci_mmcfg_region, list, &pci_mmcfg_list) {
                arch_pci_mmcfg_probe_devices(region);
        }
}
