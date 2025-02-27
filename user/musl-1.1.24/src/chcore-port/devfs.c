#include <chcore/syscall.h>

#include "devfs.h"

int chcore_virtio_file_ioctl(int fd, unsigned long request, void *arg)
{
	struct pci_dev_req *req;
	printf("request: %ld\n", request);
}

/**
 * chcore_open_dev:
 * @path: the path of the device, e.g. "/dev/xxxx"
 * @return: the file descriptor of the device
 */
int chcore_open_dev(char *path) {
	struct pci_dev_req *req;
	int segn, busn, devn, funcn;
	int ret;

	req = (struct pci_dev_req *)malloc(sizeof(struct pci_dev_req));

	if (sscanf(path, "vfio%x:%x:%x.%x", &segn, &busn, &devn, &funcn) != 4) {
		goto error;
	}

	// prepare request args
	req->req_type = PCI_CONTROL_GET_INFO;
	strncpy(req->dev_path, path, sizeof(path));
	snprintf(req->dev_ids, 11, "%04x:%02x:%02x.%01x", segn, busn, devn, funcn);

	// send request to kernel
	ret = usys_pcie_control((u64)req);
	printf("%s: ret: %d\n", __func__, ret);

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
