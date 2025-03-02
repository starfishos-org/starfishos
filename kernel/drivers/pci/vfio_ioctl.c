#include <common/kprint.h>
#include <common/errno.h>
#include <common/util.h>
#include <drivers/pci.h>
#include <drivers/vfio.h>

/**
 * vfio_handle_ioctl: ioctl handler for vfio 
 * @req_type: ioctl request type
 * @pdev: pci device
 * @args: ioctl arguments
 * @return: 0 on success, -errno on failure
 */
int vfio_handle_ioctl(u32 req_type, struct pci_dev *pdev, void *args) {
    switch (req_type) {
    case VFIO_IOMMU_MAP_DMA:
    {
        pci_ioctl_debug("vfio_iommu_map_dma\n");
        struct vfio_iommu_type1_dma_map *map = args;

        map->argsz = sizeof(struct vfio_iommu_type1_dma_map);
        map->flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE |
                     VFIO_DMA_MAP_FLAG_VADDR;
        map->iova = 0;
        map->size = 0x1000;
        map->vaddr = 0;
        return 0;
    } 
    case VFIO_DEVICE_GET_INFO:
    {
        pci_ioctl_debug("vfio_device_get_info\n");
        struct vfio_device_info *info = args;
        info->argsz = sizeof(struct vfio_device_info);
        info->flags = VFIO_DEVICE_FLAGS_PCI;
        info->num_regions = 1;
        info->num_irqs = 1;
        info->num_regions = 1;
        return 0;
    }
    case VFIO_DEVICE_GET_REGION_INFO:
    {
        pci_ioctl_debug("vfio_device_get_region_info\n");
        struct vfio_device_region_info *info = args;
        info->argsz = sizeof(struct vfio_device_region_info);
        info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
        info->offset = 0;
        info->size = 0x1000;
        return 0;
    }
    default:
        pci_ioctl_debug("vfio_ioctl: invalid request type %d\n", req_type);
        return -EINVAL;
    }
}
