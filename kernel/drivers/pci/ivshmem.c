/*
 * refer:
 * https://github.com/henning-schild-work/ivshmem-guest-code/kernel_module/standard/kvm_ivshmem.c
 */

#include <arch/mmu.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <common/errno.h>
#include <common/util.h>
#include <drivers/ivshmem.h>
#include <drivers/vfio.h>
#include <drivers/pci.h>
#include <common/bitfield.h>
#include <mm/uaccess.h>
#include <mm/vmspace.h>
#include <object/cap_group.h>
#include <object/thread.h>
#include <object/memory.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

struct kvm_ivshmem_device {
    // void * regs;

    // void * base_addr;

    // unsigned int regaddr;
    // unsigned int reg_size;

    u64 iopa;
    u64 iova;
    u64 iosize;
    // unsigned int irq;

    struct pci_dev *dev;
    // char (*msix_names)[256];
    // struct msix_entry *msix_entries;
    // int nvectors;

    bool enabled;

} __attribute__((packed, aligned(16)));

struct ivshmem_header_common {
    char magic[8];
} __attribute__((packed, aligned(PAGE_SIZE)));

struct hostfs_dev_file_info {
    u64 file_offset;
    u64 file_size;
    char file_name[128];
};

struct hostfs_dev_header {
    char magic[8];
    u64 file_num;
    struct hostfs_dev_file_info file_info_list[];
} __attribute__((packed, aligned(PAGE_SIZE)));

#define HOSTFS_PREFIX "/host/"

struct hostfs_file_info {
    u64 file_size;
    char file_name[128];
    u64 mmap_vaddr;
    u64 mmap_size;
    u64 mmap_prot;
    // the following fields are not used by kernel
    u64 fd_offset;
    u64 is_mapped;
};

// hostfs device
struct kvm_ivshmem_device *hostfs_dev;
// cxl shm device
struct kvm_ivshmem_device *cxl_shm_dev;

// ivshmem device lists
#define MAX_IVSHMEM_DEV 4
u8 kvm_ivshmem_dev_num = 0;
struct kvm_ivshmem_device kvm_ivshmem_dev_list[MAX_IVSHMEM_DEV];

void ivshmem_setup_mem(u64 *start, u64 *size)
{
    *start = cxl_shm_dev->iopa;
    *size = cxl_shm_dev->iosize;
}

extern void fill_kernel_page_table_range(u64 mem_start, u64 mem_size);
static int ivshmem_pci_probe(struct pci_dev *pdev)
{
    struct kvm_ivshmem_device *dev = &kvm_ivshmem_dev_list[kvm_ivshmem_dev_num];
    dev->iopa = pci_resource_start(pdev, 2);
    dev->iova = (u64)phys_to_virt((void *)dev->iopa);
    dev->iosize = pci_resource_len(pdev, 2);
    dev->dev = pdev;

    pci_info("[IVSHMEM] [%d] iopa=0x%llx, iova=0x%llx, iosize=0x%llx\n",
             kvm_ivshmem_dev_num,
             dev->iopa,
             dev->iova,
             dev->iosize);
    
    // Currently, the page table is not setup, so we need to refill it
    fill_kernel_page_table_range(dev->iopa, dev->iosize);

    // parse header
    struct ivshmem_header_common *header = 
        (struct ivshmem_header_common *)dev->iova;
    if (strncmp(header->magic, "hostfs", 8) == 0) {
        pci_info("[IVSHMEM] [%d] magic \"match hostfs\"\n", kvm_ivshmem_dev_num);
        hostfs_dev = dev;
    } else {
        cxl_shm_dev = dev;
    }

    kvm_ivshmem_dev_num++;

    return 0;
}

static void ivshmem_setup_dev(struct pci_dev *pdev, void *args)
{
    if (pdev->vendor == PCI_VENDOR_ID_REDHAT
        && pdev->device == PCI_DEVICE_ID_IVSHMEM) {
        // pci_info("[IVSHMEM] find ivshmem device\n");
        ivshmem_pci_probe(pdev);
    }
}

void ivshmem_setup_devices()
{
    kvm_ivshmem_dev_num = 0;
    /* probe all ivshmem devices */
    pci_buses_traverse_all(ivshmem_setup_dev, NULL);
}

static struct hostfs_dev_header *parse_hostfs_file_info() {
    return (struct hostfs_dev_header *)hostfs_dev->iova;
}

void list_hostfs_file_info() {
    struct hostfs_dev_header *header = parse_hostfs_file_info();
    pci_info("[HOSTFS] /host/: file_num=%llx\n", header->file_num);
    for (int i = 0; i < header->file_num; i++) {
        struct hostfs_dev_file_info *file_info = &header->file_info_list[i];
        pci_info("[HOSTFS] /host/%s: offset=%llx, size=%llx\n",
                 file_info->file_name,
                 file_info->file_offset,
                 file_info->file_size);
    }
}

/**
 * find_hostfs_dev_file_info(): find file info in hostfs device
 * 
 * @param file_name: file name with prefix "/host/", e.g. "/host/test.txt"
 * @return file info if found, NULL if not found
 */
struct hostfs_dev_file_info *find_hostfs_dev_file_info(char *file_name) {
    // check prefix and remove
    if (strncmp(file_name, HOSTFS_PREFIX, strlen(HOSTFS_PREFIX)) != 0) {
        pci_ioctl_debug("file name not start with %s\n", HOSTFS_PREFIX);
        return NULL;
    }
    file_name += strlen(HOSTFS_PREFIX);

    struct hostfs_dev_header *header = parse_hostfs_file_info();
    for (int i = 0; i < header->file_num; i++) {
        struct hostfs_dev_file_info *file_info = &header->file_info_list[i];
        if (strncmp(file_info->file_name, file_name, strlen(file_name)) == 0) {
            return file_info;
        }
    }
    return NULL;
}

static u64 file_offset_to_iopa(u64 file_offset) {
    u64 iopa = hostfs_dev->iopa + file_offset;
    if (iopa > hostfs_dev->iopa + hostfs_dev->iosize) {
        pci_ioctl_debug("file offset %llx out of range\n", file_offset);
        return 0;
    }

    if (iopa % PAGE_SIZE != 0) {
        pci_ioctl_debug("file offset %llx is not page aligned\n", file_offset);
        return 0;
    }

    return iopa;
}

int pci_hostfs_mmap(void *args) {
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct hostfs_file_info info;
    struct hostfs_dev_file_info *dev_file_info;
    struct vmspace *vmspace = NULL;
    struct vmregion *vmr;
    struct pmobject *pmo;
    int ret;
    u64 io_pa = 0, io_sz = 0, file_sz = 0;

    // in: mmap_vaddr, mmap_size, mmap_prot
    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct hostfs_file_info));

    dev_file_info = find_hostfs_dev_file_info(info.file_name);
    if (dev_file_info == NULL) {
        pci_ioctl_debug("file %s not found\n", req->arg_ptr);
        return -ENOENT;
    }

    io_pa = file_offset_to_iopa(dev_file_info->file_offset);
    file_sz = dev_file_info->file_size;
    io_sz = ROUND_UP(file_sz, PAGE_SIZE);

    // mmap region to user space
    ret = create_device_pmo(io_pa, io_sz, &pmo);
    if (ret < 0) {
        return ret;
    }

    // map to user space
    vmspace = get_current_vmspace();
    ret = vmspace_map_range(vmspace, 
            info.mmap_vaddr, 
            info.mmap_size, 
            info.mmap_prot, 
            pmo, 
            &vmr);
    if (ret < 0) {
        pci_ioctl_debug("vmspace_map_range failed\n");
        goto fail;
    }

    info.mmap_vaddr = vmr->start;
    info.mmap_size = io_sz;

    pci_info("[IVSHMEM] open file at dev[%d] mapped to %llx, size %llx\n",
            kvm_ivshmem_dev_num,
            info.mmap_vaddr,
            info.mmap_size);

    copy_to_user((void *)req->arg_ptr, (void *)&info, 
        sizeof(struct hostfs_file_info));

    return 0;

fail:
    pci_ioctl_debug("vmspace_map_range failed\n");
    return ret;
}

int pci_hostfs_unmap(void *args) {
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct hostfs_file_info info;
    int ret;

    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct hostfs_file_info));

    if (info.is_mapped == 0) {
        return 0;
    }

    pci_info("[IVSHMEM] unmap file %s, size %llx\n",
            info.file_name,
            info.mmap_size);
    
    ret = vmspace_unmap_range(get_current_vmspace(), 
        info.mmap_vaddr, info.mmap_size);
    if (ret < 0) {
        pci_ioctl_debug("vmspace_unmap_range failed\n");
        return ret;
    }

    return 0;
}

int pci_hostfs_open(void *args)
{
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct hostfs_file_info info;
    struct hostfs_dev_file_info *dev_file_info;
    u64 file_sz = 0;

    // in: filename
    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct hostfs_file_info));

    dev_file_info = find_hostfs_dev_file_info(info.file_name);
    if (dev_file_info == NULL) {
        pci_ioctl_debug("file %s not found\n", req->arg_ptr);
        return -ENOENT;
    }

    file_sz = dev_file_info->file_size;

    pci_info("[IVSHMEM] open file %s, size %llx\n",
            info.file_name,
            file_sz);

    // out: file_size
    copy_to_user((void *)req->arg_ptr, (void *)&info, 
        sizeof(struct hostfs_file_info));

    return 0;
}

int pci_ivshmem_close(void *args)
{
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct hostfs_file_info info;
    struct vmspace *vmspace = get_current_vmspace();
    int ret;

    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct hostfs_file_info));

    ret = vmspace_unmap_range(vmspace, info.mmap_vaddr, info.mmap_size);
    if (ret < 0) {
        pci_ioctl_debug("vmspace_unmap_range failed\n");
        return ret;
    }

    return 0;
}

int pci_hostfs_list(void *args) {
    kinfo("list hostfs file info\n");
    list_hostfs_file_info();
    return 0;
}
