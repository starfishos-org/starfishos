#include <chcore/syscall.h>

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

	switch (request) {
		case VFIO_IOMMU_MAP_DMA:
		{
			req->req_type = VFIO_IOMMU_MAP_DMA;
			strncpy(req->dev_ids, fd_dic[fd]->private_data, 
				sizeof(req->dev_ids));

			// call pcie_control to kernel
			ret = usys_pcie_control((u64)req);
			if (ret < 0) {
				goto out;
			}

			// copy return dma_map to arg
			strncpy(arg, &req->_vfio.dma_map, 
				sizeof(struct vfio_iommu_type1_dma_map));
			break;
		}
		default:
			return -EINVAL;
	}
out:
	free(req);
	return ret;
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
	.read = NULL, 
	.write = NULL, 
	.close = NULL, 
	.ioctl = chcore_virtio_file_ioctl, 
	.poll = NULL,
	.fcntl = NULL,
};
