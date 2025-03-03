/*
 * refer:
 * https://github.com/henning-schild-work/ivshmem-guest-code/kernel_module/standard/kvm_ivshmem.c
 */

#include <arch/mmu.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <common/errno.h>
#include <common/util.h>
#include <drivers/ivshmem.h>
#include <drivers/vfio.h>
#include <drivers/pci.h>
#include <common/bitfield.h>
#include <mm/uaccess.h>
#include <mm/vmspace.h>
#include <object/cap_group.h>
#include <object/thread.h>
#include <object/memory.h>

struct kvm_ivshmem_device {
    // void * regs;

    // void * base_addr;

    // unsigned int regaddr;
    // unsigned int reg_size;

    u64 ioaddr;
    u64 ioaddr_size;
    // unsigned int irq;

    struct pci_dev *dev;
    // char (*msix_names)[256];
    // struct msix_entry *msix_entries;
    // int nvectors;

    bool enabled;

} __attribute__((packed, aligned(16)));

struct hostfs_file_info {
    u64 start_vaddr;
    u64 size;
    u64 prot;
    u64 offset;
};

#define kvm_ivshmem_dev (kvm_ivshmem_dev_list[0])
u8 kvm_ivshmem_dev_num = 0;
struct kvm_ivshmem_device kvm_ivshmem_dev_list[4];

void ivshmem_setup_mem(u64 *start, u64 *size)
{
    *start = kvm_ivshmem_dev.ioaddr;
    *size = kvm_ivshmem_dev.ioaddr_size;
}

static int ivshmem_pci_probe(struct pci_dev *pdev)
{
    struct kvm_ivshmem_device *dev = &kvm_ivshmem_dev_list[kvm_ivshmem_dev_num];
    dev->ioaddr = pci_resource_start(pdev, 2);
    dev->ioaddr_size = pci_resource_len(pdev, 2);
    dev->dev = pdev;

    pci_info("[IVSHMEM] [%d] ioaddr=%llx, iosize=%llx\n",
             kvm_ivshmem_dev_num,
             dev->ioaddr,
             dev->ioaddr_size);

    kvm_ivshmem_dev_num++;

    return 0;
}

static void ivshmem_setup_dev(struct pci_dev *pdev, void *args)
{
    if (pdev->vendor == PCI_VENDOR_ID_REDHAT
        && pdev->device == PCI_DEVICE_ID_IVSHMEM) {
        // pci_info("[IVSHMEM] find ivshmem device\n");
        ivshmem_pci_probe(pdev);
    }
}

void ivshmem_setup_devices()
{
    kvm_ivshmem_dev_num = 0;
    /* probe all ivshmem devices */
    pci_buses_traverse_all(ivshmem_setup_dev, NULL);
}

int pci_ivshmem_open(void *args)
{
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct hostfs_file_info info;
    struct vmspace *vmspace = NULL;
    struct vmregion *vmr;
    struct pmobject *pmo;
    int ret;
    u64 iopa = 0, iosize = 0;

    iopa = kvm_ivshmem_dev_list[1].ioaddr;
    iosize = kvm_ivshmem_dev_list[1].ioaddr_size;

    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct hostfs_file_info));

    // mmap region to user space
    ret = create_device_pmo(iopa, iosize, &pmo);
    if (ret < 0) {
        return ret;
    }

    vmspace = get_current_vmspace();
    ret = vmspace_map_range(vmspace, 
            info.start_vaddr, 
            iosize, 
            VMR_READ, 
            pmo, 
            &vmr);
    if (ret < 0) {
        pci_ioctl_debug("vmspace_map_range failed\n");
        goto fail;
    }

    info.start_vaddr = vmr->start;
    info.size = iosize;
    info.prot = VMR_READ;
    info.offset = 0;

    pci_info("[IVSHMEM] open file at dev[%d] mapped to %llx, size %llx\n",
            kvm_ivshmem_dev_num,
            info.start_vaddr,
            info.size);

    copy_to_user((void *)req->arg_ptr, (void *)&info, 
        sizeof(struct hostfs_file_info));

    return 0;

fail:
    pci_ioctl_debug("vmspace_map_range failed\n");
    return ret;
}

int pci_ivshmem_close(void *args)
{
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct hostfs_file_info info;
    struct vmspace *vmspace = get_current_vmspace();
    int ret;

    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct hostfs_file_info));

    ret = vmspace_unmap_range(vmspace, info.start_vaddr, info.size);
    if (ret < 0) {
        pci_ioctl_debug("vmspace_unmap_range failed\n");
        return ret;
    }

    return 0;
}
