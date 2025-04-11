#include <chcore/syscall.h>
#include <chcore/pci_ioctl.h>
#include <chcore/memory.h> // for chcore_alloc_vaddr

#include "chcore_mman.h" // for READ_PROT
#include "hostfs.h"

static vaddr_t mmap_for_rw_ops(struct hostfs_file_info *info) {
	u64 mapped_vaddr;
	if (!info || !(info->pmo_cap)) {
		printf("%s: invalid info\n", __func__);
		return -ENOENT;
	}

	info->mmap_size = ROUND_UP(info->file_size, PAGE_SIZE);
	info->mmap_flags = MAP_ANONYMOUS | MAP_PRIVATE;
	info->mmap_prot = PROT_READ | PROT_WRITE;
	mapped_vaddr = (u64)chcore_mmap(0,
				info->mmap_size,
				info->mmap_prot,
				info->mmap_flags,
				-1,
				0,
				info->pmo_cap); // open will set pmo_cap
	if (mapped_vaddr == (u64)-1) {
		printf("%s: failed to mmap\n", __func__);
		return -1;
	}
	info->mmap_vaddr = mapped_vaddr;
	info->is_mapped = 1;
	return 0;
}

int chcore_hostfs_pread(int fd, void *buf, size_t count, off_t offset) {
	int ret;
	struct hostfs_file_info *info;
	
	info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	if (!info->is_mapped) {
		// printf("%s: file is not mmaped, call mmap_for_rw_ops\n", __func__);
		if ((ret = mmap_for_rw_ops(info)))
			return ret;
		// printf("mmap_for_rw_ops: fd %d, mapped vaddr: %lx, offset: %lx\n", 
		// 	fd, info->mmap_vaddr, offset);
	}
	if (offset + count > info->file_size) {
		printf("%s: invalid offset\n", __func__);
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

u64 chcore_hostfs_mmap(u64 vaddr, size_t length, int prot, int flags, int fd, off_t offset)
{
	u64 mapped_vaddr;
	struct hostfs_file_info *info;
	
	info = fd_dic[fd]->private_data;

	if (offset != 0 || length != info->file_size) {
		printf("currently chcore hostfs only support mmap whole file\n");
		offset = 0;
		length = info->file_size;
	}
	
	if (flags != (MAP_ANONYMOUS | MAP_PRIVATE)) {
		printf("currently chcore hostfs does not support flags %lx\n", flags);
		flags = MAP_ANONYMOUS | MAP_PRIVATE;
	}

	mapped_vaddr = (u64)chcore_mmap(0, length, prot, flags, -1, 0, info->pmo_cap); // open will set pmo_cap
	if (mapped_vaddr == (u64)-1) {
		printf("%s: failed to mmap\n", __func__);
		return -1;
	}
	printf("%s: fd %d, mapped vaddr: %lx, offset: %lx\n", 
		__func__, fd, mapped_vaddr, offset);
	return (long)mapped_vaddr;
}

int chcore_hostfs_open(int fd, char *path) {
	// printf("chcore_hostfs_open: %s\n", path);
	struct pci_control_req *req;
	struct hostfs_file_info *info;
	struct pci_hostfs_req_info *req_info;
	int ret;
	// prepare info
	req = malloc(sizeof(struct pci_control_req));
	req_info = malloc(sizeof(struct pci_hostfs_req_info));

	strcpy(req_info->file_name, path);
	req->req_type = PCI_CONTROL_IVSHMEM_OPEN;
	req->arg_ptr = (u64)req_info;
	req->arg_sz = sizeof(struct pci_hostfs_req_info);

	ret = usys_pcie_control((u64)req);
	if (ret < 0) {
		printf("chcore_hostfs_open: failed to open file\n");
		free(req);
		free(req_info);
		return ret;
	}

	info = malloc(sizeof(struct hostfs_file_info));
	info->file_size = req_info->file_size;
	strcpy(info->file_name, req_info->file_name);
	info->pmo_cap = req_info->pmo_cap;
	info->fd_offset = 0;
	info->is_mapped = 0;
	info->mmap_vaddr = 0;

	fd_dic[fd]->type = FD_TYPE_HOSTFS;
	fd_dic[fd]->fd_op = &hostfs_ops;
	fd_dic[fd]->private_data = info;

	printf("chcore_hostfs_open: fd %d, mapped vaddr: %lx, pmo cap: %d\n", 
		fd, info->mmap_vaddr, info->pmo_cap);

	return fd;
}

off_t chcore_hostfs_lseek(int fd, off_t offset, int whence) {
	struct hostfs_file_info *info = (struct hostfs_file_info *)fd_dic[fd]->private_data;
	switch (whence) {
		case SEEK_SET:
			info->fd_offset = offset;
			break;
		case SEEK_CUR:
			info->fd_offset += offset;
			break;
		case SEEK_END:
			info->fd_offset = info->file_size + offset;
			break;
		default:
			printf("chcore_hostfs_lseek: invalid whence %d\n", whence);
			return -EINVAL;
	}
	return info->fd_offset;
}

int chcore_hostfs_close(int fd) {
	// printf("chcore_hostfs_close\n");
	free(fd_dic[fd]->private_data);
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
	.mmap = chcore_hostfs_mmap,
};
