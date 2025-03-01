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
    int segn, busn, devn, funcn, devfn;

    // ids in format of "0000:00:00.0" (segment:bus:device.function)
    if (sscanf(ids, "%x:%x:%x.%x", &segn, &busn, &devn, &funcn) != 4) {
        return NULL;
    }
    devfn = (devn << 3) | funcn;

    // find the device that matches the ids
    for_each_in_list (pdev, struct pci_dev, bus_list, &pci_root_bus->devices) {
        if (pdev->bus->domain == segn && pdev->bus->number == busn
            && pdev->devfn == devfn) {
            return pdev;
        }
    }
    return NULL;
}