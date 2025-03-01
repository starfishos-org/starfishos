#pragma once

#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VFIO_TYPE	(';')
#define VFIO_BASE	100

/**
 * VFIO_IOMMU_MAP_DMA - _IOW(VFIO_TYPE, VFIO_BASE + 13, struct vfio_dma_map)
 *
 * Map process virtual addresses to IO virtual addresses using the
 * provided struct vfio_dma_map. Caller sets argsz. READ &/ WRITE required.
 *
 * If flags & VFIO_DMA_MAP_FLAG_VADDR, update the base vaddr for iova. The vaddr
 * must have previously been invalidated with VFIO_DMA_UNMAP_FLAG_VADDR.  To
 * maintain memory consistency within the user application, the updated vaddr
 * must address the same memory object as originally mapped.  Failure to do so
 * will result in user memory corruption and/or device misbehavior.  iova and
 * size must match those in the original MAP_DMA call.  Protection is not
 * changed, and the READ & WRITE flags must be 0.
 */
struct vfio_iommu_type1_dma_map {
	u32	argsz;
	u32	flags;
#define VFIO_DMA_MAP_FLAG_READ (1 << 0)		/* readable from device */
#define VFIO_DMA_MAP_FLAG_WRITE (1 << 1)	/* writable from device */
#define VFIO_DMA_MAP_FLAG_VADDR (1 << 2)
	u64	vaddr;				/* Process virtual address */
    u64	iova;				/* IO virtual address */
	u64	size;				/* Size of mapping (bytes) */
};

#define VFIO_IOMMU_MAP_DMA _IO(VFIO_TYPE, VFIO_BASE + 13)

/**
 * VFIO_DEVICE_GET_INFO - _IOR(VFIO_TYPE, VFIO_BASE + 7,
 *						struct vfio_device_info)
 *
 * Retrieve information about the device.  Fills in provided
 * struct vfio_device_info.  Caller sets argsz.
 * Return: 0 on success, -errno on failure.
 */
struct vfio_device_info {
	u32	argsz;
	u32	flags;
#define VFIO_DEVICE_FLAGS_RESET	(1 << 0)	/* Device supports reset */
#define VFIO_DEVICE_FLAGS_PCI	(1 << 1)	/* vfio-pci device */
#define VFIO_DEVICE_FLAGS_PLATFORM (1 << 2)	/* vfio-platform device */
#define VFIO_DEVICE_FLAGS_AMBA  (1 << 3)	/* vfio-amba device */
#define VFIO_DEVICE_FLAGS_CCW	(1 << 4)	/* vfio-ccw device */
#define VFIO_DEVICE_FLAGS_AP	(1 << 5)	/* vfio-ap device */
#define VFIO_DEVICE_FLAGS_FSL_MC (1 << 6)	/* vfio-fsl-mc device */
#define VFIO_DEVICE_FLAGS_CAPS	(1 << 7)	/* Info supports caps */
#define VFIO_DEVICE_FLAGS_CDX	(1 << 8)	/* vfio-cdx device */
	u32	num_regions;	/* Max region index + 1 */
	u32	num_irqs;	/* Max IRQ index + 1 */
	u32 cap_offset;	/* Offset within info struct of first cap */
	u32 pad;
};
#define VFIO_DEVICE_GET_INFO		_IO(VFIO_TYPE, VFIO_BASE + 7)

/**
 * VFIO_DEVICE_GET_REGION_INFO - _IOWR(VFIO_TYPE, VFIO_BASE + 8,
 *				       struct vfio_region_info)
 *
 * Retrieve information about a device region.  Caller provides
 * struct vfio_region_info with index value set.  Caller sets argsz.
 * Implementation of region mapping is bus driver specific.  This is
 * intended to describe MMIO, I/O port, as well as bus specific
 * regions (ex. PCI config space).  Zero sized regions may be used
 * to describe unimplemented regions (ex. unimplemented PCI BARs).
 * Return: 0 on success, -errno on failure.
 */
struct vfio_region_info {
	u32	argsz;
	u32	flags;
#define VFIO_REGION_INFO_FLAG_READ	(1 << 0) /* Region supports read */
#define VFIO_REGION_INFO_FLAG_WRITE	(1 << 1) /* Region supports write */
#define VFIO_REGION_INFO_FLAG_MMAP	(1 << 2) /* Region supports mmap */
#define VFIO_REGION_INFO_FLAG_CAPS	(1 << 3) /* Info supports caps */
	u32	index;		/* Region index */
	u32	cap_offset;	/* Offset within info struct of first cap */
	u64	size;		/* Region size (bytes) */
	u64	offset;		/* Region offset from start of device fd */
};
#define VFIO_DEVICE_GET_REGION_INFO	_IO(VFIO_TYPE, VFIO_BASE + 8)


#ifdef __cplusplus
}
#endif
