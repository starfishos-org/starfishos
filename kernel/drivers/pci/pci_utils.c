#include <common/kprint.h>
#include <common/util.h>
#include <drivers/pci.h>
#include <drivers/io.h>

extern struct pci_bus *pci_root_bus;

void pci_devices_traverse(struct pci_bus *bus, pci_bus_traverse_fn func,
                          void *args)
{
    struct pci_dev *pdev;
    for_each_in_list (pdev, struct pci_dev, bus_list, &bus->devices) {
        func(pdev, args);
    }
}

void pci_buses_traverse(struct pci_bus *parent, pci_bus_traverse_fn func,
                        void *args)
{
    struct pci_bus *child;
    pci_devices_traverse(parent, func, args);

    for_each_in_list (child, struct pci_bus, node, &parent->children) {
        pci_buses_traverse(child, func, args);
    }
}

void pci_buses_traverse_all(pci_bus_traverse_fn func, void *args)
{
    pci_buses_traverse(pci_root_bus, func, args);
}

struct pci_dev *pci_find_device_by_vendor_and_device_id(u16 vendor, u16 device)
{
    struct pci_dev *pdev;
    for_each_in_list (pdev, struct pci_dev, bus_list, &pci_root_bus->devices) {
        if (pdev->vendor == vendor && pdev->device == device) {
            return pdev;
        }
    }
    return NULL;
}

struct pci_dev *pci_find_device_by_ids(char *ids)
{
    struct pci_dev *pdev;
    char real_ids[11];

    for_each_in_list (pdev, struct pci_dev, bus_list, &pci_root_bus->devices) {
        snprintf(real_ids,
                 sizeof(real_ids),
                 "%04x:%02x:%02x.%01x",
                 pdev->bus->domain,
                 pdev->bus->number,
                 pdev->devfn >> 3,
                 pdev->devfn & 0x7);
        if (strncmp(real_ids, ids, 11) == 0) {
            return pdev;
        }
    }
    return NULL;
}