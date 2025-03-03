#include <chcore/syscall.h>
#include <chcore/pci_ioctl.h>
#include <chcore/memory.h> // for chcore_alloc_vaddr

#include "chcore_mman.h" // for READ_PROT
#include "hostfs.h"

int chcore_hostfs_pread(int fd, void *buf, size_t count, off_t offset) {
	struct hostfs_file_info *info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	if (offset + count > info->file_size) {
		printf("chcore_hostfs_pread: invalid offset\n");
		return -EINVAL;
	}
	memcpy(buf, (void *)(info->mmap_vaddr + offset), count);
	return count;
}

int chcore_hostfs_read(int fd, void *buf, size_t count) {
	struct hostfs_file_info *info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	off_t offset = info->fd_offset;
	int ret = chcore_hostfs_pread(fd, buf, count, offset);
	if (ret < 0) {
		printf("chcore_hostfs_read: failed to read\n");
		return ret;
	}
	info->fd_offset += ret;
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
	struct pci_control_req *req = malloc(sizeof(struct pci_control_req));
	struct hostfs_file_info *info = malloc(sizeof(struct hostfs_file_info));
	int ret;

	// open file
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
			info->fd_offset = offset;
			printf("chcore_hostfs_lseek: SEEK_SET ret=%lx\n", info->fd_offset);
			break;
		case SEEK_CUR:
			info->fd_offset += offset;
			printf("chcore_hostfs_lseek: SEEK_CUR ret=%lx\n", info->fd_offset);
			break;
		case SEEK_END:
			info->fd_offset = info->file_size + offset;
			printf("chcore_hostfs_lseek: SEEK_END ret=%lx\n", info->fd_offset);
			break;
		default:
			printf("chcore_hostfs_lseek: invalid whence %d\n", whence);
			return -EINVAL;
	}
	return info->fd_offset;
}

int chcore_hostfs_close(int fd) {
	printf("chcore_hostfs_close\n");
	free(fd_dic[fd]->private_data);
	fd_dic[fd]->private_data = NULL;
	return 0;
}

long chcore_hostfs_mmap(u64 vaddr, size_t length, int prot, int flags, int fd, off_t offset)
{
	printf("chcore_hostfs_mmap: %lx, %lx, %d, %d, %d, %lx\n", vaddr, length, prot, flags, fd, offset);
	struct hostfs_file_info *info = 
			(struct hostfs_file_info *)fd_dic[fd]->private_data;
	struct pci_control_req *req = malloc(sizeof(struct pci_control_req));
	u64 mmap_size = 0;
	int ret = 0;
	
	if (info->is_mapped) {
		return (long)info->mmap_vaddr + offset;
	}

	// prepare info
	mmap_size = ROUND_UP(info->file_size, PAGE_SIZE);
	info->mmap_vaddr = chcore_alloc_vaddr(mmap_size);
	info->mmap_size = mmap_size;
	info->mmap_prot = prot;

	// mmap file
	req->req_type = PCI_CONTROL_IVSHMEM_MMAP;
	req->arg_ptr = (u64)info;
	req->arg_sz = sizeof(struct hostfs_file_info);

	ret = usys_pcie_control((u64)req);
	if (ret < 0) {
		free(req);
		free(info);
		return ret;
	}

	info->is_mapped = 1;

	return (long)info->mmap_vaddr + offset;
}

struct fd_ops hostfs_ops = {
	// .open = chcore_hostfs_open,
	.read = chcore_hostfs_read, 
	.pread = chcore_hostfs_pread, 
	.write = chcore_hostfs_write,
	.pwrite = chcore_hostfs_pwrite,
	.lseek = chcore_hostfs_lseek,
	.close = chcore_hostfs_close,
	.mmap = chcore_hostfs_mmap,
};
