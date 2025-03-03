#include <common/kprint.h>
#include <drivers/pci.h>
#include <mm/uaccess.h>
#include <mm/kmalloc.h>
#include <common/errno.h>
#include <common/util.h>
#include <drivers/ivshmem.h>

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
int pci_list_all_devices(struct pci_control_req *req)
{
    // struct pci_dev *dev;
    (void)req;
    // kinfo("[PCI] list all devices:\n");
    pci_buses_traverse_all(__fn_pci_info_dev, NULL);
    return 0;
}

int pci_device_map_region(struct pci_control_req *req)
{
    (void)req;
    return 0;
}

int hostfs_handle_ioctl(struct pci_control_req *req)
{
    switch (req->req_type)
    {
    case PCI_CONTROL_IVSHMEM_OPEN:
        return pci_hostfs_open(req);
    case PCI_CONTROL_IVSHMEM_MMAP:
        return pci_hostfs_mmap(req);
    case PCI_CONTROL_IVSHMEM_UNMAP:
        return pci_hostfs_unmap(req);
    case PCI_CONTROL_IVSHMEM_LIST:
        return pci_hostfs_list(req);
    default:
        printk("unknown ioctl type %lx\n", req->req_type);
        break;
    }
    return -EINVAL;
}

int sys_pcie_control(u64 usr_req_buf)
{
    struct pci_control_req *req;
    u64 device_type;
    int ret = 0;

    req = (struct pci_control_req *)kmalloc(
        sizeof(struct pci_control_req), __PRIVATE__);
    if (!req) {
        ret = -ENOMEM;
        goto out;
    }

    copy_from_user((char *)req, (char *)usr_req_buf, sizeof(struct pci_control_req));
    // pci_ioctl_debug("[PCI] sys_pcie_control request is %lx\n", req->req_type);

    if (req->req_type == PCI_CONTROL_LIST_DEVICES) {
        ret = pci_list_all_devices(req);
        goto out;
    }
    
    if (req->req_type == PCI_CONTROL_OPEN_DEVICE) {
        struct pci_dev *pdev = pci_find_device_by_ids(req->dev_ids);
        if (!pdev) {
            pci_info("device %s not found\n", req->dev_ids);
            ret = -ENODEV;
        }
        goto out;
    }

    // ioctl requests are related to each devices
    device_type = _IOC_TYPE(req->req_type);

    switch (device_type) {
    case HOSTFS_TYPE: {
        return hostfs_handle_ioctl(req);
    }
    case VFIO_TYPE: {
        struct pci_dev *pdev = pci_find_device_by_ids(req->dev_ids);
        if (!pdev) {
            pci_info("device /dev/%s not found\n", req->dev_ids);
            ret = -ENODEV;
            goto out;
        }
        ret = vfio_handle_ioctl(req->req_type, pdev, 
            req->arg_ptr, req->arg_sz);
        goto out;
    }

    default:
        pci_info("Chcore does not support device type %c\n", device_type);
        ret = -EINVAL;
        goto out;
    }

out:
    kfree(req);
    return ret;
}
