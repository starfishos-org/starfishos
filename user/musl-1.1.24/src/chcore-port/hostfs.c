#include <chcore/syscall.h>
#include <chcore/pci_ioctl.h>

#include "hostfs.h"

int chcore_hostfs_pread(int fd, void *buf, size_t count, off_t offset) {
	struct hostfs_file_info *info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	if (offset + count > info->size) {
		printf("chcore_hostfs_pread: invalid offset\n");
		return -EINVAL;
	}
	memcpy(buf, (void *)(info->start_vaddr + offset), count);
	return count;
}

int chcore_hostfs_read(int fd, void *buf, size_t count) {
	struct hostfs_file_info *info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	off_t offset = info->offset;
	int ret = chcore_hostfs_pread(fd, buf, count, offset);
	if (ret < 0) {
		printf("chcore_hostfs_read: failed to read\n");
		return ret;
	}
	info->offset += ret;
	return ret;
}

int chcore_hostfs_pwrite(int fd, void *buf, size_t count, off_t offset) {
	printf("chcore_hostfs_pwrite not supported\n");
	return -1;
}

int chcore_hostfs_write(int fd, void *buf, size_t count) {
	printf("chcore_hostfs_write not supported\n");
	return -1;
}

int chcore_hostfs_open(int fd, char *path) {
	printf("chcore_hostfs_open: %s\n", path);
	struct pci_control_req *req;
	struct hostfs_file_info *info;
	int ret;
	req = malloc(sizeof(struct pci_control_req));
	info = malloc(sizeof(struct hostfs_file_info));
	info->start_vaddr = HOSTFS_VADDR;

	req->req_type = PCI_CONTROL_IVSHMEM_OPEN;
	req->arg_ptr = (u64)info;
	req->arg_sz = sizeof(struct hostfs_file_info);

	ret = usys_pcie_control((u64)req);
	if (ret < 0) {
		free(req);
		free(info);
		return ret;
	}

	fd_dic[fd]->type = FD_TYPE_HOSTFS;
	fd_dic[fd]->fd_op = &hostfs_ops;
	fd_dic[fd]->private_data = info;

	return fd;
}

off_t chcore_hostfs_lseek(int fd, off_t offset, int whence) {
	struct hostfs_file_info *info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	switch (whence) {
		case SEEK_SET:
			info->offset = offset;
			printf("chcore_hostfs_lseek: SEEK_SET ret=%lx\n", info->offset);
			break;
		case SEEK_CUR:
			info->offset += offset;
			printf("chcore_hostfs_lseek: SEEK_CUR ret=%lx\n", info->offset);
			break;
		case SEEK_END:
			info->offset = info->size + offset;
			printf("chcore_hostfs_lseek: SEEK_END ret=%lx\n", info->offset);
			break;
		default:
			printf("chcore_hostfs_lseek: invalid whence %d\n", whence);
			return -EINVAL;
	}
	return info->offset;
}

int chcore_hostfs_close(int fd) {
	printf("chcore_hostfs_close\n");
	struct pci_control_req *req = malloc(sizeof(struct pci_control_req));
	struct hostfs_file_info *info = 
			(struct hostfs_file_info *)fd_dic[fd]->private_data;
	int ret = 0;

	req->req_type = PCI_CONTROL_IVSHMEM_CLOSE;
	req->arg_ptr = (u64)info;
	req->arg_sz = sizeof(struct hostfs_file_info);

	ret = usys_pcie_control((u64)req);
	if (ret < 0) {
		free(req);
		return ret;
	}

	free(req);
	free(info);
	fd_dic[fd]->private_data = NULL;
	return 0;
}

struct fd_ops hostfs_ops = {
	// .open = chcore_hostfs_open,
	.read = chcore_hostfs_read, 
	.pread = chcore_hostfs_pread, 
	.write = chcore_hostfs_write,
	.pwrite = chcore_hostfs_pwrite,
	.lseek = chcore_hostfs_lseek,
	.close = chcore_hostfs_close,
};
