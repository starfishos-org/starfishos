#include <chcore/syscall.h>
#include <chcore/vfio.h>

#include "devfs.h"

static int path_to_ids(char *path, char *ids) {
	int segn, busn, devn, funcn;
	// FIXME(FN): currently fixed to "/dev/xxx" pattern
	if (sscanf(path, "/dev/%x:%x:%x.%x", &segn, &busn, &devn, &funcn) != 4) {
		return -EINVAL;
	}
	snprintf(ids, 13, "%04x:%02x:%02x.%01x", segn, busn, devn, funcn);
	return 0;
}

int chcore_virtio_file_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;
	struct pci_control_req *req;

	req = (struct pci_control_req *)malloc(sizeof(struct pci_control_req));
	if (!req) {
		return -ENOMEM;
	}

	req->req_type = request;
	strncpy(req->dev_ids, fd_dic[fd]->private_data, 
		sizeof(req->dev_ids));
	// printf("VFIO_IOMMU_MAP_DMA 0x%lx\n", VFIO_IOMMU_MAP_DMA);
	// printf("req->req_type 0x%lx\n", req->req_type);

	switch (req->req_type) {
		case VFIO_IOMMU_MAP_DMA:
		{
			// printf("VFIO_IOMMU_MAP_DMA\n");
			req->arg_ptr = (u64)arg;
			req->arg_sz = sizeof(struct vfio_iommu_type1_dma_map);
			ret = usys_pcie_control((u64)req);
			// printf("VFIO_IOMMU_MAP_DMA finished\n");
			break;
		}
		case VFIO_DEVICE_GET_INFO:
		{
			// printf("VFIO_DEVICE_GET_INFO\n");
			req->arg_ptr = (u64)arg;
			req->arg_sz = sizeof(struct vfio_device_info);
			ret = usys_pcie_control((u64)req);
			break;
		}
		case VFIO_DEVICE_GET_REGION_INFO:
		{
			// printf("VFIO_DEVICE_GET_REGION_INFO\n");
			req->arg_ptr = (u64)arg;
			req->arg_sz = sizeof(struct vfio_region_info);
			ret = usys_pcie_control((u64)req);
			break;
		}
		default:
			printf("VFIO ioctl not supported %lx\n", request);
			return -EINVAL;
	}
out:
	free(req);
	return ret;
}

int chcore_virtio_file_pread(int fd, void *buf, size_t count, off_t offset) {
	printf("chcore_virtio_file_read\n");
	return 0;
}

int chcore_virtio_file_pwrite(int fd, void *buf, size_t count, off_t offset) {
	printf("chcore_virtio_file_write\n");
	return 0;
}

/**
 * chcore_open_dev:
 * @path: the path of the device, e.g. "/dev/xxxx"
 * @return: the file descriptor of the device
 */
int chcore_open_dev(int fd, char *path) {
	struct pci_control_req *req;
	int ret;

	req = (struct pci_control_req *)malloc(sizeof(struct pci_control_req));

	// prepare request args
	req->req_type = PCI_CONTROL_OPEN_DEVICE;
	if ((path_to_ids(path, req->dev_ids)) < 0) {
		goto error;
	}

	// send request to kernel
	ret = usys_pcie_control((u64)req);
	if (ret < 0) {
		goto error;
	}

	fd_dic[fd]->type = FD_TYPE_DEV;
	fd_dic[fd]->fd_op = &virtio_file_ops;
	// vfio: set fd_dic[fd]->private_data to req->dev_ids
	fd_dic[fd]->private_data = malloc(sizeof(req->dev_ids));
	if (!fd_dic[fd]->private_data) {
		ret = -ENOMEM;
		goto error;
	}
	strncpy(fd_dic[fd]->private_data, req->dev_ids, 
			sizeof(req->dev_ids));
	ret = fd;

error:
	free(req);
	return ret;
}

struct fd_ops virtio_file_ops = {
	.pread = chcore_virtio_file_pread, 
	.pwrite = chcore_virtio_file_pwrite,
	.ioctl = chcore_virtio_file_ioctl,
};
