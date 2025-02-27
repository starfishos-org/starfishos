/*
 * refer:
 * https://github.com/henning-schild-work/ivshmem-guest-code/kernel_module/standard/kvm_ivshmem.c
 */

#include <arch/mmu.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <drivers/cxl.h>
#include <drivers/pci.h>
#include <common/errno.h>
#include <common/bitfield.h>
#include <drivers/pci-special.h>

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

struct kvm_ivshmem_device kvm_ivshmem_dev;

void ivshmem_setup_mem(u64 *start, u64 *size)
{
    *start = kvm_ivshmem_dev.ioaddr;
    *size = kvm_ivshmem_dev.ioaddr_size;
}

static int ivshmem_pci_probe(struct pci_dev *pdev)
{
    kvm_ivshmem_dev.ioaddr = pci_resource_start(pdev, 2);
    kvm_ivshmem_dev.ioaddr_size = pci_resource_len(pdev, 2);
    kvm_ivshmem_dev.dev = pdev;

    pci_info("[IVSHMEM] ioaddr=%llx, iosize=%llx\n",
             kvm_ivshmem_dev,
             kvm_ivshmem_dev.ioaddr,
             kvm_ivshmem_dev.ioaddr_size);

    return 0;
}

static void ivshmem_setup_dev(struct pci_dev *pdev, void *args)
{
    if (pdev->vendor == PCI_VENDOR_ID_IVSHMEM
        && pdev->device == PCI_DEVICE_ID_QEMU_IVSHMEM) {
        pci_info("[IVSHMEM] find ivshmem device\n");
        ivshmem_pci_probe(pdev);
    }
}

void ivshmem_setup_devices()
{
    /* probe all ivshmem devices */
    pci_buses_traverse_all(ivshmem_setup_dev, NULL);
}
