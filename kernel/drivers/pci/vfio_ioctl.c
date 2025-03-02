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

#if 0
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

    pci_ioctl_debug("map->iova: %lx, map->size: %lx, map->vaddr: %lx\n", 
            map->iova, map->size, map->vaddr);
    // ret = create_device_pmo(map->iova, map->size, &pmo);
    ret = create_pmo(map->size, PMO_DATA, __DEFAULT__, 
            current_cap_group, &pmo);
    if (ret < 0) {
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
#endif // virtual memory is already allocated by user space

static int vfio_do_map_dma(struct vfio_iommu_type1_dma_map *map, struct pci_dev *pdev)
{
    return 0;
}

static int vfio_iommu_type1_map_dma(struct pci_dev *pdev, struct vfio_iommu_type1_dma_map *map)
{
	unsigned long minsz;
    u32 mask = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE |
            VFIO_DMA_MAP_FLAG_VADDR;
    minsz = sizeof(struct vfio_iommu_type1_dma_map);

	if (map->argsz < minsz || map->flags & ~mask)
		return -EINVAL;

	return vfio_do_map_dma(map, pdev);
}

/**
 * vfio_handle_ioctl: ioctl handler for vfio 
 * @req_type: ioctl request type
 * @pdev: pci device
 * @arg_ptr: ioctl arguments
 * @arg_sz: ioctl arguments size
 * @return: 0 on success, -errno on failure
 */
int vfio_handle_ioctl(u32 req_type, struct pci_dev *pdev, u64 arg_ptr, u64 arg_sz) {
    int ret = 0;
    void *kargs = kmalloc(arg_sz, __DEFAULT__);
    if (!kargs) {
        return -ENOMEM;
    }
    copy_from_user(kargs, (void *)arg_ptr, arg_sz);

    switch (req_type) {
    case VFIO_IOMMU_MAP_DMA:
    {
        pci_ioctl_debug("vfio_iommu_map_dma\n");
        ret = vfio_iommu_type1_map_dma(pdev, 
            (struct vfio_iommu_type1_dma_map *)kargs);
        break;
    } 
    case VFIO_DEVICE_GET_INFO:
    {
        pci_ioctl_debug("vfio_device_get_info\n");
        struct vfio_device_info *info = (struct vfio_device_info *)kargs;
        info->flags = VFIO_DEVICE_FLAGS_PCI;
        info->num_regions = 9;
        info->num_irqs = 1;
        break;
    }
    case VFIO_DEVICE_GET_REGION_INFO:
    {
        pci_ioctl_debug("vfio_device_get_region_info\n");
        struct vfio_region_info *info = (struct vfio_region_info *)kargs;
        info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
        info->index = 0;
        info->cap_offset = 0;
        info->size = 0x1000;
        info->offset = 0;
        break;
    }
    default:
        pci_ioctl_debug("vfio_ioctl: invalid request type %d\n", req_type);
        kfree(kargs);
        return -EINVAL;
    }

    copy_to_user((void *)arg_ptr, kargs, arg_sz);
    kfree(kargs);
    return ret;
}
