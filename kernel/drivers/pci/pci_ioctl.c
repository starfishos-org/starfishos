#include <common/kprint.h>
#include <drivers/pci.h>
#include <drivers/io.h>
#include <mm/uaccess.h>
#include <mm/kmalloc.h>
#include <common/errno.h>
#include <common/util.h>

/* syscall for userspace */
/* functions for pci control */
void __fn_pci_info_dev(struct pci_dev *pdev, void *args)
{
    (void)args;
    kinfo("%04x:%02x:%02x.%x %s: %s %s [%04x:%04x]\n",
          pdev->bus->domain,
          pdev->bus->number,
          pdev->devfn >> 3,
          pdev->devfn & 0x7,
          pci_class_name(pdev->class),
          pci_vendor_name(pdev->vendor),
          pci_device_name(pdev->vendor, pdev->device),
          pdev->vendor,
          pdev->device);
}

/*
 * pci_list_all_devices: list all pci devices
 */
int pci_list_all_devices(struct pci_dev_req *req)
{
    // struct pci_dev *dev;
    (void)req;
    // kinfo("[PCI] list all devices:\n");
    pci_buses_traverse_all(__fn_pci_info_dev, NULL);
    return 0;
}

/*
 * pci_device_get_info: find device by vendor and device id
 * @args: vendor and device id
 * @pdev: struct pci_dev *pdev
 */
int pci_device_get_info(struct pci_dev_req *req)
{
    struct pci_dev *pdev = pci_find_device_by_ids(req->dev_ids);
    if (!pdev) {
        pci_info("device %s not found\n", req->dev_path);
        return -ENODEV;
    }
    // copy pdev info to user space
    // copy_to_user(req->ret, pdev, sizeof(struct pci_dev));
    return 0;
}

int pci_device_map_region(struct pci_dev_req *req)
{
    (void)req;
    return 0;
}

int sys_pcie_control(u64 usr_req_buf)
{
    struct pci_dev_req *req;
    int ret = 0;

    req = (struct pci_dev_req *)kmalloc(
        sizeof(struct pci_dev_req), __PRIVATE__);
    if (!req) {
        ret = -ENOMEM;
        goto error;
    }

    copy_from_user((char *)req, (char *)usr_req_buf, sizeof(struct pci_dev_req));
    pci_ioctl_debug("[PCI] sys_pcie_control request is %s\n", pci_control_type_str[req->req_type]);

    switch (req->req_type) {
    case PCI_CONTROL_LIST_DEVICES: 
        ret = pci_list_all_devices(req);
        break;
    case PCI_CONTROL_GET_INFO:
        ret = pci_device_get_info(req);
        break;
    case PCI_CONTROL_MAP_REGION:
        ret = pci_device_map_region(req);
        break;
    default:
        pci_info("No such control type\n");
        break;
    }

error:
    kfree(req);
    return ret;
}
