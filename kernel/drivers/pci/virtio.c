#include <arch/mmu.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <drivers/virtio.h>
#include <drivers/pci.h>
#include <common/errno.h>
#include <common/bitfield.h>
#include <drivers/pci-special.h>

// static int virtio_pci_probe(struct pci_dev *pdev)
// {
//         kvm_ivshmem_dev.ioaddr = pci_resource_start(pdev, 2);
//         kvm_ivshmem_dev.ioaddr_size = pci_resource_len(pdev, 2);
//         kvm_ivshmem_dev.dev = pdev;

//         pci_info("[IVSHMEM] ioaddr=%llx, iosize=%llx\n",
//                 kvm_ivshmem_dev,
//                 kvm_ivshmem_dev.ioaddr,
//                 kvm_ivshmem_dev.ioaddr_size);

//         return 0;
// }

static void virtio_setup_dev(struct pci_dev *pdev)
{
        // if (pdev->vendor == PCI_VENDOR_ID_VIRTIO
        //         && pdev->device == PCI_DEVICE_ID_VIRTIO) {
        //                 pci_info("[VIRTIO] find virtio device\n");
        //                 virtio_pci_probe(pdev);
        // }
}

void ivshmem_setup_devices()
{
        /* probe all cxl devices and set hdm decoder */
        pci_buses_traverse_all(virtio_setup_dev);
}
