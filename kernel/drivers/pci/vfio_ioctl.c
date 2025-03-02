#include <common/kprint.h>
#include <common/errno.h>
#include <common/util.h>
#include <mm/uaccess.h>
#include <mm/vmspace.h>
#include <object/memory.h>
#include <drivers/pci.h>
#include <drivers/vfio.h>
#include <object/cap_group.h>
#include <object/thread.h>

static int vfio_do_map_dma(struct vfio_iommu_type1_dma_map *map, struct pci_dev *pdev)
{
    // bool set_vaddr = map->flags & VFIO_DMA_MAP_FLAG_VADDR;
    // vaddr_t iova = map->iova;
	unsigned long vaddr = map->vaddr;
	size_t size = map->size;
	int ret = 0, prot = 0;
	// size_t pgsize;
    // paddr_t iopa;
    struct vmspace *vmspace = NULL;
    struct vmregion *vmr = NULL;
    struct pmobject *pmo = NULL;

	/* READ/WRITE from device perspective */
	if (map->flags & VFIO_DMA_MAP_FLAG_WRITE)
		prot |= VMR_WRITE;
	if (map->flags & VFIO_DMA_MAP_FLAG_READ)
		prot |= VMR_READ;

	// if ((prot && set_vaddr) || (!prot && !set_vaddr))
	// 	return -EINVAL;

    // get paddr of device memory
    // if (iova) {
    //     iopa = vaddr_to_paddr(iova);
    // }

    pci_ioctl_debug("map->iova: %lx, map->size: %lx\n", map->iova, map->size);
    // ret = create_device_pmo(map->iova, map->size, &pmo);
    ret = create_pmo(map->size, PMO_DATA, __DEFAULT__, 
            current_cap_group, &pmo);
    if (ret) {
        pci_ioctl_debug("create_pmo failed\n");
        return ret;
    }

    vmspace = get_current_vmspace();
    pci_ioctl_debug("vmspace:%lx, vaddr:%lx, size:%lx, prot:%lx, pmo:%lx\n", vmspace, vaddr, size, prot, pmo);
    ret = vmspace_map_range(vmspace, vaddr, size, prot, pmo, &vmr);
    if (ret == -EINVAL) {
        // retry with va = 0
        ret = vmspace_map_range(vmspace, -1, size, prot, pmo, &vmr);
        if (ret < 0) {
            pci_ioctl_debug("vmspace_map_range failed\n");
            goto fail;
        }
        map->vaddr = vmr->start;
        pci_ioctl_debug("vmspace:%lx, vaddr:%lx, size:%lx, prot:%lx, pmo:%lx\n", vmspace, vaddr, size, prot, pmo);
    } else if (ret < 0) {
        goto fail;
    }
    return 0;
fail:
    pci_ioctl_debug("vmspace_map_range failed\n");
    return ret;
}

static int vfio_iommu_type1_map_dma(struct vfio_iommu_type1_dma_map *map, struct pci_dev *pdev)
{
	// unsigned long minsz;
	// uint32_t mask = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE |
	// 		VFIO_DMA_MAP_FLAG_VADDR;
    // minsz = sizeof(struct vfio_iommu_type1_dma_map);

	// if (map.argsz < minsz || map.flags & ~mask)
	// 	return -EINVAL;

	return vfio_do_map_dma(map, pdev);
}

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
        int ret = vfio_iommu_type1_map_dma(
            (struct vfio_iommu_type1_dma_map *)args, pdev);
        return ret;
    } 
    case VFIO_DEVICE_GET_INFO:
    {
        pci_ioctl_debug("vfio_device_get_info\n");
        struct vfio_device_info *info = args;
        info->argsz = sizeof(struct vfio_device_info);
        info->flags = VFIO_DEVICE_FLAGS_PCI;
        info->num_regions = 9;
        info->num_irqs = 1;
        return 0;
    }
    case VFIO_DEVICE_GET_REGION_INFO:
    {
        pci_ioctl_debug("vfio_device_get_region_info\n");
        struct vfio_region_info *info = args;
        info->argsz = sizeof(struct vfio_region_info);
        info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
        info->index = 0;
        info->cap_offset = 0;
        info->size = 0x1000;
        info->offset = 0;
        return 0;
    }
    default:
        pci_ioctl_debug("vfio_ioctl: invalid request type %d\n", req_type);
        return -EINVAL;
    }
}
