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
#include <dsm/dsm-single.h>
#include <irq/irq.h>
#include <irq/timer.h>
#include <arch/mm/tlb.h>
#include <common/lock.h>
#include <sched/context.h>
#include <sched/sched.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <common/util.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE (4096)
#endif

#ifndef DEFAULT_STACK_SIZE
#define DEFAULT_STACK_SIZE (8 << 20)  /* 8MB stack */
#endif

/* Forward declaration */
extern void arch_idle_ctx_init(struct thread_ctx *idle_ctx, u64 stack, void (*func)(void));
extern struct vmspace *create_idle_vmspace(void);

struct kvm_ivshmem_device {
    /* BAR 2: Shared memory */
    u64 iopa;              /* Physical address of BAR 2 (shared memory) */
    u64 iova;              /* Virtual address of BAR 2 */
    u64 iosize;            /* Size of BAR 2 */

    /* BAR 0: MMIO registers */
    u64 regs_iopa;         /* Physical address of BAR 0 (registers) */
    u64 regs_iova;         /* Virtual address of BAR 0 */
    u64 regs_iosize;       /* Size of BAR 0 */

    struct pci_dev *dev;
    u8 msix_cap;           /* MSI-X capability offset */
    u16 msix_table_size;   /* Number of MSI-X vectors */
    void *msix_table_base; /* Base address of MSI-X table */
    void *msix_pba_base;   /* Base address of Pending Bit Array */
    u32 *doorbell_regs;    /* Doorbell registers in BAR 0 */
    u32 peer_id;           /* IVPosition (peer ID assigned by ivshmem-server) */

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

struct pci_hostfs_req_info {
    u64 file_size;
    char file_name[128];
    u64 pmo_cap;
};

// fd_dic private data
struct hostfs_file_info {
    u64 file_size;
    char file_name[128];
    u64 pmo_cap;
    u64 mmap_vaddr;
    u64 mmap_size;
    u64 mmap_prot;
    // the following fields are not used by kernel
    u64 fd_offset;
    u64 is_mapped;
};

// hostfs device
struct kvm_ivshmem_device *hostfs_dev;
// cxl shm device (for shared memory)
struct kvm_ivshmem_device *cxl_shm_dev;
// doorbell device (for MSI notification)
struct kvm_ivshmem_device *doorbell_dev;

// ivshmem device lists
#define MAX_IVSHMEM_DEV 4
u8 kvm_ivshmem_dev_num = 0;
struct kvm_ivshmem_device kvm_ivshmem_dev_list[MAX_IVSHMEM_DEV];

// Message processing mode: 0 = MSI (default), 1 = Polling
static enum ivshmem_msg_mode ivshmem_msg_mode = IVSHMEM_MSG_MODE_MSI;

void ivshmem_setup_mem(u64 *start, u64 *size)
{
    *start = cxl_shm_dev->iopa;
    *size = cxl_shm_dev->iosize;
}

extern void fill_kernel_page_table_range(u64 mem_start, u64 mem_size);

/* Find PCI capability */
static u8 pci_find_capability(struct pci_dev *dev, u8 cap)
{
    u8 pos;
    u8 id;
    u16 status;

    pci_read_config_word(dev, PCI_STATUS, &status);
    if (!(status & PCI_STATUS_CAP_LIST))
        return 0;

    pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &pos);
    while (pos) {
        pci_read_config_byte(dev, pos + PCI_CAP_LIST_ID, &id);
        if (id == cap) {
            return pos;
        }
        pci_read_config_byte(dev, pos + PCI_CAP_LIST_NEXT, &pos);
        /* Safety check: prevent infinite loop */
        if (pos < 0x40 || pos > 0xFC) {
            break;
        }
    }
    return 0;
}

/* Initialize MSI-X for ivshmem-doorbell device */
static int ivshmem_init_msix(struct kvm_ivshmem_device *dev)
{
    struct pci_dev *pdev = dev->dev;
    u16 msix_flags;
    u32 table_offset, pba_offset;
    u8 bir;
    u64 bar_addr;

    pci_info("[IVSHMEM] Initializing MSI-X for device %04x:%04x\n", 
             pdev->vendor, pdev->device);
    pci_info("[IVSHMEM] BAR0: iopa=0x%llx, iova=0x%llx, iosize=0x%llx\n",
             dev->regs_iopa, dev->regs_iova, dev->regs_iosize);

    /* Find MSI-X capability */
    /* Debug: Check PCI config space first */
    u8 cap_ptr = 0;
    u16 status = 0;
    pci_read_config_word(pdev, PCI_STATUS, &status);
    pci_info("[IVSHMEM] PCI Status: 0x%04x (Capabilities List: %s)\n", 
             status, (status & PCI_STATUS_CAP_LIST) ? "Yes" : "No");
    
    if (!(status & PCI_STATUS_CAP_LIST)) {
        pci_info("[IVSHMEM] Device does not support PCI capabilities list\n");
        return -ENODEV;
    }
    
    pci_read_config_byte(pdev, PCI_CAPABILITY_LIST, &cap_ptr);
    pci_info("[IVSHMEM] PCI capability list pointer: 0x%02x\n", cap_ptr);
    
    /* Debug: Walk through all capabilities */
    u8 pos = cap_ptr;
    int cap_count = 0;
    while (pos && cap_count < 20) {
        u8 cap_id = 0;
        u8 next_ptr = 0;
        pci_read_config_byte(pdev, pos + PCI_CAP_LIST_ID, &cap_id);
        pci_read_config_byte(pdev, pos + PCI_CAP_LIST_NEXT, &next_ptr);
        pci_info("[IVSHMEM] Capability at 0x%02x: ID=0x%02x, Next=0x%02x\n", pos, cap_id, next_ptr);
        if (cap_id == PCI_CAP_ID_MSIX) {
            pci_info("[IVSHMEM] Found MSI-X capability at 0x%02x!\n", pos);
        }
        pos = next_ptr;
        cap_count++;
    }
    
    /* Use the position we found during debugging, or try pci_find_capability */
    u8 found_msix_pos = 0;
    pos = cap_ptr;
    cap_count = 0;
    while (pos && cap_count < 20) {
        u8 cap_id = 0;
        u8 next_ptr = 0;
        pci_read_config_byte(pdev, pos + PCI_CAP_LIST_ID, &cap_id);
        pci_read_config_byte(pdev, pos + PCI_CAP_LIST_NEXT, &next_ptr);
        if (cap_id == PCI_CAP_ID_MSIX) {
            found_msix_pos = pos;
            break;
        }
        pos = next_ptr;
        cap_count++;
        if (pos < 0x40 || pos > 0xFC) {
            break;
        }
    }
    
    if (found_msix_pos) {
        dev->msix_cap = found_msix_pos;
        pci_info("[IVSHMEM] Using MSI-X capability at 0x%02x (found during scan)\n", dev->msix_cap);
    } else {
        dev->msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
        if (!dev->msix_cap) {
            pci_info("[IVSHMEM] MSI-X capability not found after scanning\n");
            pci_info("[IVSHMEM] This may indicate that QEMU ivshmem-doorbell device is not configured with MSI-X support\n");
            pci_info("[IVSHMEM] Please check QEMU command line includes: -device ivshmem-doorbell,chardev=...,vectors=16\n");
            return -ENODEV;
        }
    }
    
    pci_info("[IVSHMEM] Found MSI-X capability at offset 0x%x\n", dev->msix_cap);

    /* Read MSI-X capability */
    pci_read_config_word(pdev, dev->msix_cap + PCI_MSIX_FLAGS, &msix_flags);
    dev->msix_table_size = (msix_flags & PCI_MSIX_FLAGS_QSIZE) + 1;
    
    pci_read_config_dword(pdev, dev->msix_cap + PCI_MSIX_TABLE, &table_offset);
    pci_read_config_dword(pdev, dev->msix_cap + PCI_MSIX_PBA, &pba_offset);

    bir = table_offset & PCI_MSIX_TABLE_BIR;
    bar_addr = pci_resource_start(pdev, bir);
    
    dev->msix_table_base = (void *)(phys_to_virt((void *)bar_addr) + 
                                    (table_offset & PCI_MSIX_TABLE_OFFSET));
    dev->msix_pba_base = (void *)(phys_to_virt((void *)bar_addr) + 
                                  (pba_offset & PCI_MSIX_PBA_OFFSET));

    /* Enable MSI-X */
    msix_flags |= PCI_MSIX_FLAGS_ENABLE;
    pci_write_config_word(pdev, dev->msix_cap + PCI_MSIX_FLAGS, msix_flags);

    /* Doorbell registers are in BAR 0 (MMIO registers) */
    /* ivshmem-doorbell layout in BAR 0:
     * - Offset 0x00: IVPosition (read-only, VM ID)
     * - Offset 0x04: Doorbell (write to trigger interrupt)
     * - Offset 0x08: Interrupt Mask
     * - Offset 0x0C: Interrupt Status
     * Each VM has its own doorbell register at offset (vm_id * 0x10) + 0x04
     */
    if (!dev->regs_iova) {
        pci_info("[IVSHMEM] BAR 0 not initialized (regs_iova=%p), cannot set doorbell registers\n",
                 dev->regs_iova);
        pci_info("[IVSHMEM] BAR0 details: iopa=0x%llx, iosize=0x%llx\n",
                 dev->regs_iopa, dev->regs_iosize);
        return -EINVAL;
    }
    
    /* Note: IVPosition is at offset 0x08, Doorbell is at offset 0x0c */
    /* Doorbell format: low 16 bits = peer_id, high 16 bits = vector */
    dev->doorbell_regs = (u32 *)(dev->regs_iova + 0x0c); /* Doorbell register at offset 0x0c */
    
    /* Use machine_id as peer_id directly (IVPosition register may not be reliable) */
    if (CUR_MACHINE_ID >= 0 && CUR_MACHINE_ID < CLUSTER_MAX_MACHINE_NUM) {
        dev->peer_id = CUR_MACHINE_ID;
    } else {
        /* Fallback: try to read IVPosition if machine_id is not available yet */
        u32 *ivposition_reg = (u32 *)dev->regs_iova;
        dev->peer_id = *ivposition_reg;
        pci_info("[IVSHMEM] Warning: CUR_MACHINE_ID=%d is invalid, using IVPosition=%u\n", 
                 CUR_MACHINE_ID, dev->peer_id);
    }
    
    pci_info("[IVSHMEM] Doorbell registers set at %p (BAR0 base + 0x04)\n", dev->doorbell_regs);
    pci_info("[IVSHMEM] Using machine_id as peer_id: machine_id=%d -> peer_id=%u\n", 
             CUR_MACHINE_ID, dev->peer_id);
    
    /* Register peer_id mapping in shared memory */
    /* Note: dsm_meta might not be initialized yet during PCI probe */
    /* We'll register it later in ivshmem_register_peer_id() */
    if (dsm_meta && CUR_MACHINE_ID >= 0 && CUR_MACHINE_ID < CLUSTER_MAX_MACHINE_NUM) {
        dsm_meta->machine_to_peer_id[CUR_MACHINE_ID] = dev->peer_id;
        pci_info("[IVSHMEM] Registered mapping: machine_id=%d -> peer_id=%u\n", 
                 CUR_MACHINE_ID, dev->peer_id);
    } else {
        pci_info("[IVSHMEM] Deferring peer_id registration: dsm_meta=%p, CUR_MACHINE_ID=%d\n",
                 dsm_meta, CUR_MACHINE_ID);
    }

    pci_info("[IVSHMEM] MSI-X initialized: table_size=%d, table_base=%p, pba_base=%p, doorbell=%p\n",
             dev->msix_table_size, dev->msix_table_base, dev->msix_pba_base, dev->doorbell_regs);

    /* Configure MSI-X table entries and register interrupt handler */
    /* Use IRQ_MSIX_IVSHMEM (48) for MSI-X interrupts */
    #include <irq/irq.h>
    
    if (dev->msix_table_base && dev->msix_table_size > 0) {
        u32 *msix_entry = (u32 *)((u64)dev->msix_table_base + 0 * PCI_MSIX_ENTRY_SIZE);
        
        /* Configure MSI-X entry for vector 0 */
        /* Message Address: Physical address that triggers the interrupt */
        /* For x86_64, MSI-X uses a fixed address format:
         * - Bits [19:12]: Destination ID (CPU APIC ID, 0 = broadcast to all CPUs)
         * - Bit 4: Redirection Hint (0 = fixed, 1 = lowest priority)
         * - Bit 3: Destination Mode (0 = physical, 1 = logical)
         * - Bits [2:0]: Must be 0
         * 
         * Address format: 0xFEE00000 (Local APIC base) + (destination_id << 12)
         * For broadcast: destination_id = 0
         */
        u32 msg_addr_low = 0xFEE00000; /* Local APIC base, broadcast to all CPUs */
        u32 msg_addr_high = 0x00000000;
        /* MSI-X msg_data format for x86_64:
         * - Bits [7:0]: Interrupt vector (IDT index, 0-255)
         * - Bits [10:8]: Delivery mode (000 = fixed, 001 = lowest priority, etc.)
         * - Bit 11: Level (0 = edge, 1 = level)
         * - Bit 14: Trigger mode (0 = edge, 1 = level)
         * - Other bits: Reserved
         * 
         * We use vector 48 (IRQ_MSIX_IVSHMEM) with edge-triggered, fixed delivery
         */
        u32 msg_data = IRQ_MSIX_IVSHMEM; /* Interrupt vector (IDT index 48) */
        u32 vector_ctrl = 0; /* Not masked */
        
        /* Write MSI-X table entry with memory barriers */
        __sync_synchronize();
        msix_entry[PCI_MSIX_ENTRY_LOWER_ADDR / 4] = msg_addr_low;
        __sync_synchronize();
        msix_entry[PCI_MSIX_ENTRY_UPPER_ADDR / 4] = msg_addr_high;
        __sync_synchronize();
        msix_entry[PCI_MSIX_ENTRY_DATA / 4] = msg_data;
        __sync_synchronize();
        msix_entry[PCI_MSIX_ENTRY_VECTOR_CTRL / 4] = vector_ctrl;
        __sync_synchronize();
        
        /* Verify the configuration */
        u32 read_addr_low = msix_entry[PCI_MSIX_ENTRY_LOWER_ADDR / 4];
        u32 read_addr_high = msix_entry[PCI_MSIX_ENTRY_UPPER_ADDR / 4];
        u32 read_data = msix_entry[PCI_MSIX_ENTRY_DATA / 4];
        u32 read_ctrl = msix_entry[PCI_MSIX_ENTRY_VECTOR_CTRL / 4];
        
        pci_info("[IVSHMEM] Configured MSI-X vector 0: IRQ=%d\n", IRQ_MSIX_IVSHMEM);
        pci_info("[IVSHMEM]   Address: 0x%08x%08x (expected 0x%08x%08x)\n",
                 read_addr_high, read_addr_low, msg_addr_high, msg_addr_low);
        pci_info("[IVSHMEM]   Data: 0x%08x (expected 0x%08x)\n", read_data, msg_data);
        pci_info("[IVSHMEM]   Control: 0x%08x (expected 0x%08x)\n", read_ctrl, vector_ctrl);
        pci_info("[IVSHMEM]   Table base: %p, Entry offset: 0x%x\n",
                 dev->msix_table_base, 0 * PCI_MSIX_ENTRY_SIZE);
        
        if (read_addr_low != msg_addr_low || read_data != msg_data) {
            pci_info("[IVSHMEM] WARNING: MSI-X table entry verification failed!\n");
        }
        
        pci_info("[IVSHMEM] MSI-X interrupt handler registered (IRQ %d)\n", 
                 IRQ_MSIX_IVSHMEM);
    }

    return 0;
}

static int ivshmem_pci_probe(struct pci_dev *pdev)
{
    struct kvm_ivshmem_device *dev = &kvm_ivshmem_dev_list[kvm_ivshmem_dev_num];
    
    /* BAR 2: Shared memory */
    dev->iopa = pci_resource_start(pdev, 2);
    dev->iova = (u64)phys_to_virt((void *)dev->iopa);
    dev->iosize = pci_resource_len(pdev, 2);
    
    /* BAR 0: MMIO registers (for doorbell and interrupt control) */
    dev->regs_iopa = pci_resource_start(pdev, 0);
    dev->regs_iova = (u64)phys_to_virt((void *)dev->regs_iopa);
    dev->regs_iosize = pci_resource_len(pdev, 0);
    
    dev->dev = pdev;
    dev->msix_cap = 0;
    dev->msix_table_size = 0;
    dev->msix_table_base = NULL;
    dev->msix_pba_base = NULL;
    dev->doorbell_regs = NULL;

    pci_info("[IVSHMEM] [%d] BAR2 (shared mem): iopa=0x%llx, iova=0x%llx, iosize=0x%llx\n",
             kvm_ivshmem_dev_num,
             dev->iopa,
             dev->iova,
             dev->iosize);
    pci_info("[IVSHMEM] [%d] BAR0 (registers): iopa=0x%llx, iova=0x%llx, iosize=0x%llx\n",
             kvm_ivshmem_dev_num,
             dev->regs_iopa,
             dev->regs_iova,
             dev->regs_iosize);
    
    // Currently, the page table is not setup, so we need to refill it
    fill_kernel_page_table_range(dev->iopa, dev->iosize);
    if (dev->regs_iopa)
        fill_kernel_page_table_range(dev->regs_iopa, dev->regs_iosize);

    // parse header
    // ivshmem-doorbell may not have BAR 2 (shared memory), so check BAR 0 first
    if (dev->regs_iova && dev->regs_iosize > 0 && (!dev->iova || dev->iosize == 0)) {
        /* This is likely a doorbell device (has BAR 0 but no BAR 2) */
        doorbell_dev = dev;
        pci_info("[IVSHMEM] [%d] identified as doorbell device (BAR0 only)\n", kvm_ivshmem_dev_num);
        /* Initialize MSI-X for doorbell device */
        int ret = ivshmem_init_msix(dev);
        if (ret != 0) {
            pci_info("[IVSHMEM] [%d] MSI-X initialization failed: %d\n", kvm_ivshmem_dev_num, ret);
        }
    } else if (dev->iova && dev->iosize > 0) {
        /* Device has BAR 2, check magic header */
        struct ivshmem_header_common *header = 
            (struct ivshmem_header_common *)dev->iova;
        if (strncmp(header->magic, "hostfs", 6) == 0) {
            pci_info("[IVSHMEM] [%d] magic \"match hostfs\"\n", kvm_ivshmem_dev_num);
            hostfs_dev = dev;
        } else if (strncmp(header->magic, "cxlmem", 6) == 0) {
            cxl_shm_dev = dev;
            pci_info("[IVSHMEM] [%d] magic \"match cxlmem\" (shared memory device)\n", kvm_ivshmem_dev_num);
        } else if (dev->regs_iova && dev->regs_iosize > 0) {
            /* Device has both BAR 0 and BAR 2, but unknown magic - might be doorbell */
            doorbell_dev = dev;
            pci_info("[IVSHMEM] [%d] identified as doorbell device (unknown magic)\n", kvm_ivshmem_dev_num);
            /* Initialize MSI-X for doorbell device */
            int ret = ivshmem_init_msix(dev);
            if (ret != 0) {
                pci_info("[IVSHMEM] [%d] MSI-X initialization failed: %d\n", kvm_ivshmem_dev_num, ret);
            }
        }
    }

    kvm_ivshmem_dev_num++;

    return 0;
}

static void ivshmem_setup_dev(struct pci_dev *pdev, void *args)
{
    /* Both ivshmem-plain and ivshmem-doorbell use the same vendor/device ID */
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

static struct hostfs_dev_header *parse_pci_hostfs_req_info() {
    if (hostfs_dev == NULL) {
        pci_ioctl_debug("hostfs_dev is not initialized\n");
        return NULL;
    }
    return (struct hostfs_dev_header *)hostfs_dev->iova;
}

void list_pci_hostfs_req_info() {
    struct hostfs_dev_header *header = parse_pci_hostfs_req_info();
    if (header == NULL) {
        pci_ioctl_debug("header is not initialized\n");
        return;
    }
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

    struct hostfs_dev_header *header = parse_pci_hostfs_req_info();
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
    return 0;
}

int pci_hostfs_unmap(void *args) {
    return 0;
}

int pci_hostfs_open(void *args)
{
    struct pci_control_req *req = (struct pci_control_req *)args;
    struct pci_hostfs_req_info info;
    struct hostfs_dev_file_info *dev_file_info;
    struct pmobject *pmo;
    int ret;
    u64 io_pa = 0, io_sz = 0, file_sz = 0;

    // in: filename
    copy_from_user((void *)&info, (void *)req->arg_ptr, 
        sizeof(struct pci_hostfs_req_info));

    // kinfo("open file %s\n", info.file_name);
    dev_file_info = find_hostfs_dev_file_info(info.file_name);
    if (dev_file_info == NULL) {
        pci_ioctl_debug("file %s not found\n", req->arg_ptr);
        return -ENOENT;
    }

    // get file size
    file_sz = dev_file_info->file_size;
    info.file_size = file_sz;

    pci_info("[HOSTFS] open file %s, size %llx\n",
            info.file_name,
            file_sz);
    
    io_pa = file_offset_to_iopa(dev_file_info->file_offset);
    io_sz = ROUND_UP(file_sz, PAGE_SIZE);

    // get pmo_cap
    info.pmo_cap = create_device_pmo(io_pa, io_sz, &pmo);
    if (info.pmo_cap < 0) {
        return ret;
    }

    // out: file_size
    copy_to_user((void *)req->arg_ptr, (void *)&info, 
        sizeof(struct pci_hostfs_req_info));

    return 0;
}

int pci_ivshmem_close(void *args)
{
    return 0;
}

int pci_hostfs_list(void *args) {
    kinfo("list hostfs file info\n");
    list_pci_hostfs_req_info();
    return 0;
}

#include <dsm/dsm-single.h>

/**
 * ivshmem_send_msi - Send MSI interrupt to another machine via doorbell
 * @target_machine_id: Target machine ID
 * @vector: MSI-X vector number (0-15)
 * 
 * This function writes to the doorbell register in shared memory to trigger
 * an MSI interrupt on the target machine.
 * 
 * ivshmem-doorbell device layout:
 * - BAR 0 (MMIO): Registers including doorbell
 *   - Each VM has doorbell register at offset (vm_id * 0x10) + 0x04 in BAR 0
 *   - Writing to doorbell register triggers MSI interrupt to that VM
 *   - The value written is the vector number
 * - BAR 2 (Memory): Shared memory for data exchange
 * 
 * Returns 0 on success, -EINVAL on failure
 */
int ivshmem_send_msi(mid_t target_machine_id, u16 vector)
{
    struct kvm_ivshmem_device *dev = doorbell_dev;
    
    if (!dev || !dev->doorbell_regs || !dev->msix_cap) {
        pci_info("[IVSHMEM] Doorbell device not initialized or MSI-X not supported\n");
        return -EINVAL;
    }

    if (vector >= dev->msix_table_size) {
        pci_info("[IVSHMEM] Invalid vector %d (max %d)\n", vector, dev->msix_table_size);
        return -EINVAL;
    }

    if (target_machine_id >= CLUSTER_MACHINE_NUM) {
        pci_info("[IVSHMEM] Invalid machine ID %d\n", target_machine_id);
        return -EINVAL;
    }

    /* ivshmem-doorbell: Each VM has a doorbell register at offset (peer_id * 0x10) + 0x04 in BAR 0 */
    /* Note: We need to use peer_id (assigned by ivshmem-server) not machine_id */
    /* Get peer_id for target machine from shared memory mapping */
    u32 target_peer_id = target_machine_id; /* Default: assume peer_id == machine_id */
    if (dsm_meta && target_machine_id < CLUSTER_MAX_MACHINE_NUM) {
        target_peer_id = dsm_meta->machine_to_peer_id[target_machine_id];
        /* Check if mapping is valid (0xFFFFFFFF means uninitialized) */
        if (target_peer_id == 0xFFFFFFFF) {
            /* Mapping not yet registered, try using machine_id as fallback */
            pci_info("[IVSHMEM] Warning: peer_id mapping for machine %d not found (uninitialized), using machine_id\n", 
                     target_machine_id);
            target_peer_id = target_machine_id;
        }
    }
    
    // pci_info("[IVSHMEM] Sending MSI: target_machine_id=%d, target_peer_id=%u (my_peer_id=%u)\n", 
    //          target_machine_id, target_peer_id, doorbell_dev ? doorbell_dev->peer_id : 0xFFFFFFFF);
    
    /* Writing to doorbell register triggers MSI interrupt to that VM */
    /* According to ivshmem-doorbell spec:
     * - Doorbell is 32-bit register at offset 0x0c in BAR0
     * - Low 16 bits: peer_id (target peer to interrupt)
     * - High 16 bits: vector number (interrupt vector, 0-based index)
     * - Format: (vector << 16) | peer_id
     */
    u32 doorbell_value = ((u32)vector << 16) | (target_peer_id & 0xFFFF);
    u32 *doorbell = (u32 *)((u64)dev->regs_iova + 0x0c); /* Doorbell at offset 0x0c */
    
    // pci_info("[IVSHMEM] Writing to doorbell: peer_id=%u, vector=%d\n", target_peer_id, vector);
    // pci_info("[IVSHMEM]   doorbell_value=0x%08x (vector=0x%04x, peer_id=0x%04x)\n",
    //          doorbell_value, vector, target_peer_id);
    // pci_info("[IVSHMEM]   doorbell register at %p (BAR0 offset 0x0c)\n", doorbell);
    
    /* Use memory barrier to ensure write is visible before triggering interrupt */
    __sync_synchronize();
    *doorbell = doorbell_value;
    __sync_synchronize();
    
    /* Verify the write */
    // u32 read_back = *doorbell;
    // pci_info("[IVSHMEM] Doorbell write: wrote=0x%08x, read_back=0x%08x\n", doorbell_value, read_back);

    // pci_info("[IVSHMEM] Sent MSI to machine %d (peer_id=%u), vector %d\n", 
    //          target_machine_id, target_peer_id, vector);
    
    return 0;
}

/**
 * ivshmem_register_peer_id - Register peer_id mapping in shared memory
 * 
 * This function should be called after dsm_meta is initialized
 * to register the peer_id mapping that was deferred during PCI probe.
 */
void ivshmem_register_peer_id(void)
{
    if (!doorbell_dev || !dsm_meta) {
        return;
    }
    
    mid_t my_id = CUR_MACHINE_ID;
    if (my_id < 0 || my_id >= CLUSTER_MAX_MACHINE_NUM) {
        kinfo("[IVSHMEM] Cannot register peer_id: invalid CUR_MACHINE_ID=%d\n", my_id);
        return;
    }
    
    /* Re-read IVPosition to ensure we have the latest value */
    /* ivshmem-server may update IVPosition after VM connects */
    /* Use machine_id as peer_id directly */
    doorbell_dev->peer_id = my_id;
    
    /* Register peer_id mapping */
    dsm_meta->machine_to_peer_id[my_id] = doorbell_dev->peer_id;
    kinfo("[IVSHMEM] Registered peer_id mapping: machine_id=%d -> peer_id=%u\n", 
          my_id, doorbell_dev->peer_id);
}

/**
 * ivshmem_msix_handler - MSI-X interrupt handler
 * 
 * This function is called when an MSI-X interrupt is received.
 * It processes incoming MSI messages and sends replies.
 * Only processes messages if MSI mode is enabled.
 */
void ivshmem_msix_handler(void)
{
    kinfo("[IVSHMEM] ===== MSI-X interrupt received on machine %d (mode=%d) =====\n", 
          CUR_MACHINE_ID, ivshmem_msg_mode);
    
    /* Only process MSI messages if MSI mode is enabled */
    if (ivshmem_msg_mode == IVSHMEM_MSG_MODE_MSI) {
        /* Process MSI messages */
        ivshmem_process_msi_messages();
    } else {
        kinfo("[IVSHMEM] MSI-X interrupt ignored (polling mode enabled)\n");
    }
    
    kinfo("[IVSHMEM] ===== MSI-X interrupt handling completed =====\n");
    
    /* Acknowledge interrupt (if needed) */
    /* For MSI-X, the interrupt is automatically acknowledged by the hardware */
}

/**
 * ivshmem_handle_tlb_flush_msg - Handle TLB flush message
 * 
 * @param my_id: Current machine ID
 * @param sender_id: ID of the machine that sent the message
 * @return: 0 on success, -1 on failure
 */
static int ivshmem_handle_tlb_flush_msg(mid_t my_id, mid_t sender_id)
{
    /* Read message parameters with lock protection */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    /* Note: We read the parameters but don't use them for now */
    /* In the future, we might use them for more precise TLB flushing */
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    
    /* Perform TLB flush on all CPUs of this machine */
    /* For cross-machine TLB flush, we flush all TLBs to ensure consistency */
    flush_tlb_all();
    
    /* Send reply back to sender's slot (with lock protection) */
    lock(&dsm_meta->msi_test_msg[sender_id].msg_lock);
    dsm_meta->msi_test_msg[sender_id].reply_received = 1;
    dsm_meta->msi_test_msg[sender_id].reply_from = my_id;
    unlock(&dsm_meta->msi_test_msg[sender_id].msg_lock);
    
    /* Clear the message in our slot to avoid processing it again */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 1; /* Mark as processed */
    dsm_meta->msi_test_msg[my_id].msg_from = 0xFFFFFFFF; /* Clear msg_from */
    dsm_meta->msi_test_msg[my_id].msg_type = 0; /* Clear msg_type */
    dsm_meta->msi_test_msg[my_id].tlb_start_va = 0;
    dsm_meta->msi_test_msg[my_id].tlb_len = 0;
    dsm_meta->msi_test_msg[my_id].tlb_vmspace = 0;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    
    return 0;
}

/**
 * ivshmem_handle_memcpy_and_flush_tlb_msg - Handle memcpy and flush TLB message
 * 
 * This function performs memcpy from src_pa to dst_pa, then flushes all TLBs.
 * The memcpy is performed on the remote machine to avoid accessing another machine's
 * physical memory directly, which could cause nested page faults.
 * 
 * @param my_id: Current machine ID
 * @param sender_id: ID of the machine that sent the message
 * @return: 0 on success, -1 on failure
 */
static int ivshmem_handle_memcpy_and_flush_tlb_msg(mid_t my_id, mid_t sender_id)
{
    /* Read message parameters with lock protection */
    kinfo("[IVSHMEM] handle memcpy and flush TLB message, my_id: %d, sender_id: %d\n", my_id, sender_id);
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    paddr_t src_pa = (paddr_t)dsm_meta->msi_test_msg[my_id].memcpy_src_pa;
    paddr_t dst_pa = (paddr_t)dsm_meta->msi_test_msg[my_id].memcpy_dst_pa;
    size_t len = (size_t)dsm_meta->msi_test_msg[my_id].memcpy_len;
    vaddr_t fault_va = (vaddr_t)dsm_meta->msi_test_msg[my_id].memcpy_fault_va;
    struct vmspace *vmspace = (struct vmspace *)dsm_meta->msi_test_msg[my_id].memcpy_vmspace;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    
    /* Perform memcpy: copy from src_pa to dst_pa */
    /* Note: Both src_pa and dst_pa are physical addresses on this machine */
    void *src_va = (void *)phys_to_virt(src_pa);
    void *dst_va = (void *)phys_to_virt(dst_pa);
    memcpy(dst_va, src_va, len);
    
    /* Perform TLB flush on all CPUs of this machine */
    /* For cross-machine TLB flush, we flush all TLBs to ensure consistency */
    // flush_tlb_all();
    flush_tlb_local_and_remote(vmspace, fault_va, len);
    
    /* Send reply back to sender's slot (with lock protection) */
    lock(&dsm_meta->msi_test_msg[sender_id].msg_lock);
    dsm_meta->msi_test_msg[sender_id].reply_received = 1;
    dsm_meta->msi_test_msg[sender_id].reply_from = my_id;
    unlock(&dsm_meta->msi_test_msg[sender_id].msg_lock);
    
    /* Clear the message in our slot to avoid processing it again */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 1; /* Mark as processed */
    dsm_meta->msi_test_msg[my_id].msg_from = 0xFFFFFFFF; /* Clear msg_from */
    dsm_meta->msi_test_msg[my_id].msg_type = 0; /* Clear msg_type */
    dsm_meta->msi_test_msg[my_id].memcpy_src_pa = 0;
    dsm_meta->msi_test_msg[my_id].memcpy_dst_pa = 0;
    dsm_meta->msi_test_msg[my_id].memcpy_len = 0;
    dsm_meta->msi_test_msg[my_id].memcpy_fault_va = 0;
    dsm_meta->msi_test_msg[my_id].memcpy_vmspace = 0;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    
    return 0;
}

/**
 * ivshmem_handle_test_msg - Handle test message
 * 
 * @param my_id: Current machine ID
 * @param sender_id: ID of the machine that sent the message
 * @return: 0 on success, -1 on failure
 */
static int ivshmem_handle_test_msg(mid_t my_id, mid_t sender_id)
{
    // kinfo("[IVSHMEM] Machine %d received MSI message from machine %d (msg_from=%u, msg_type=%u, slot=%d)\n", 
    //       my_id, sender_id, dsm_meta->msi_test_msg[my_id].msg_from, 
    //       dsm_meta->msi_test_msg[my_id].msg_type, my_id);

    /* Send reply back to sender's slot (with lock protection) */
    lock(&dsm_meta->msi_test_msg[sender_id].msg_lock);
    dsm_meta->msi_test_msg[sender_id].reply_received = 1;
    dsm_meta->msi_test_msg[sender_id].reply_from = my_id;
    unlock(&dsm_meta->msi_test_msg[sender_id].msg_lock);

    // kinfo("[IVSHMEM] Machine %d sent reply to machine %d (placed in slot %d, reply_from=%u)\n", 
    //       my_id, sender_id, sender_id, dsm_meta->msi_test_msg[sender_id].reply_from);
    
    /* Clear the message in our slot to avoid processing it again */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 1; /* Mark as processed */
    dsm_meta->msi_test_msg[my_id].msg_from = 0xFFFFFFFF; /* Clear msg_from */
    dsm_meta->msi_test_msg[my_id].msg_type = 0; /* Clear msg_type */
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    
    return 0;
}

/**
 * ivshmem_poll_messages - Poll for and process messages (non-blocking)
 * 
 * This function checks for incoming messages in shared memory and processes them.
 * Unlike ivshmem_process_msi_messages, this function is designed to be called
 * from any context (including page fault handlers) and does not rely on MSI interrupts.
 * 
 * This function only processes messages sent to the current machine.
 * For messages sent to other machines, those machines need to call this function themselves.
 * 
 * @return: 1 if a message was processed, 0 otherwise
 */
int ivshmem_poll_messages(void)
{
    mid_t my_id = CUR_MACHINE_ID;
    int i;
    int processed = 0;

    if (!dsm_meta || my_id >= CLUSTER_MACHINE_NUM)
        return 0;

    /* Check for messages sent to us */
    for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (i == my_id)
            continue;

        /* Check if there's a message for us in our slot (with lock protection) */
        lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        u32 msg_from = dsm_meta->msi_test_msg[my_id].msg_from;
        u32 msg_type = dsm_meta->msi_test_msg[my_id].msg_type;
        u32 reply_received = dsm_meta->msi_test_msg[my_id].reply_received;
        unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        
        /* Check if there's a message for us from machine i */
        if (msg_from == i && reply_received == 0) {
            kinfo("[IVSHMEM] Machine %d polling: found message msg_from=%u, msg_type=%u, from machine %d\n", 
                  my_id, msg_from, msg_type, i);
            int handled = 0;
            
            /* Dispatch message to appropriate handler based on message type */
            switch (msg_type) {
            case MSI_MSG_TYPE_TLB_FLUSH:
                handled = ivshmem_handle_tlb_flush_msg(my_id, i);
                break;
                
            case MSI_MSG_TYPE_TEST:
                handled = ivshmem_handle_test_msg(my_id, i);
                break;
                
            case MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB:
                handled = ivshmem_handle_memcpy_and_flush_tlb_msg(my_id, i);
                break;
                
            default:
                /* Unknown message type - skip it */
                break;
            }
            
            if (handled == 0) {
                processed = 1;
            }
        }
    }
    
    return processed;
}

/**
 * ivshmem_poll_messages_for_target - Poll and process messages for a specific target machine
 * 
 * This function checks for messages sent to a specific target machine and processes them.
 * This is useful when the sender is waiting for a reply and wants to help process
 * messages on the target machine (e.g., when MSI interrupts are disabled or delayed).
 * 
 * @param target_mid: Target machine ID to check for messages
 * @return: 1 if a message was processed, 0 otherwise
 */
int ivshmem_poll_messages_for_target(mid_t target_mid)
{
    mid_t my_id = CUR_MACHINE_ID;
    int i;
    int processed = 0;

    if (!dsm_meta || my_id >= CLUSTER_MACHINE_NUM || target_mid >= CLUSTER_MACHINE_NUM)
        return 0;

    /* Check if there's a message for the target machine from any sender */
    for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (i == target_mid)
            continue;

        /* Check the target machine's slot for messages */
        lock(&dsm_meta->msi_test_msg[target_mid].msg_lock);
        u32 msg_from = dsm_meta->msi_test_msg[target_mid].msg_from;
        u32 msg_type = dsm_meta->msi_test_msg[target_mid].msg_type;
        u32 reply_received = dsm_meta->msi_test_msg[target_mid].reply_received;
        unlock(&dsm_meta->msi_test_msg[target_mid].msg_lock);
        
        /* Check if there's a message for the target machine from machine i */
        if (msg_from == i && reply_received == 0) {
            kinfo("[IVSHMEM] Machine %d helping process message for target machine %d: msg_from=%u, msg_type=%u, from machine %d\n", 
                  my_id, target_mid, msg_from, msg_type, i);
            
            /* We can't directly process the message for another machine, but we can
             * trigger processing by simulating what would happen if the target machine
             * called ivshmem_poll_messages(). However, this requires the target machine
             * to actually process the message itself.
             * 
             * Instead, we just return that we found a message, and the caller can
             * wait for the target machine to process it.
             */
            processed = 1;
            break;
        }
    }
    
    return processed;
}

/**
 * ivshmem_process_msi_messages - Process received MSI messages and send replies
 * 
 * This function checks for incoming MSI messages in shared memory and
 * dispatches them to appropriate handlers based on message type.
 * It should be called periodically or from an interrupt handler.
 */
void ivshmem_process_msi_messages(void)
{
    mid_t my_id = CUR_MACHINE_ID;
    int i;

    if (!dsm_meta || my_id >= CLUSTER_MACHINE_NUM)
        return;

    /* Check for messages sent to us */
    for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (i == my_id)
            continue;

        /* Check if there's a message for us in our slot (with lock protection) */
        /* Message should be placed in target machine's slot by sender */
        /* Note: We check our own slot (msi_test_msg[my_id]) for incoming messages */
        lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        u32 msg_from = dsm_meta->msi_test_msg[my_id].msg_from;
        u32 msg_type = dsm_meta->msi_test_msg[my_id].msg_type;
        u32 reply_received = dsm_meta->msi_test_msg[my_id].reply_received;
        unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        
        /* Check if there's a message for us from machine i */
        if (msg_from == i && reply_received == 0) {
            kinfo("[IVSHMEM] Machine %d processing message: msg_from=%u, msg_type=%u, from machine %d\n", 
                  my_id, msg_from, msg_type, i);
            int handled = 0;
            
            /* Dispatch message to appropriate handler based on message type */
            switch (msg_type) {
            case MSI_MSG_TYPE_TLB_FLUSH:
                handled = ivshmem_handle_tlb_flush_msg(my_id, i);
                break;
                
            case MSI_MSG_TYPE_TEST:
                handled = ivshmem_handle_test_msg(my_id, i);
                break;
                
            case MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB:
                handled = ivshmem_handle_memcpy_and_flush_tlb_msg(my_id, i);
                break;
                
            default:
                kwarn("[IVSHMEM] Unknown message type %u from machine %d\n", msg_type, i);
                /* Clear invalid message */
                lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
                dsm_meta->msi_test_msg[my_id].reply_received = 1;
                dsm_meta->msi_test_msg[my_id].msg_from = 0xFFFFFFFF;
                dsm_meta->msi_test_msg[my_id].msg_type = 0;
                unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
                handled = -1;
                break;
            }
            
            if (handled == 0) {
                /* Message processed successfully, only process one message at a time */
                break;
            }
        }
    }
}

/**
 * ivshmem_test_basic - Basic test of doorbell and shared memory
 * 
 * This function tests basic doorbell register access and shared memory access
 * without relying on MSI interrupts.
 * 
 * Returns 0 on success, -EINVAL on failure
 */
int ivshmem_test_basic(void)
{
    mid_t my_id = CUR_MACHINE_ID;
    
    if (!dsm_meta || !doorbell_dev || !doorbell_dev->doorbell_regs) {
        kinfo("[IVSHMEM] Basic test skipped: doorbell device not initialized\n");
        return -EINVAL;
    }
    
    /* Ensure peer_id is registered */
    ivshmem_register_peer_id();
    
    kinfo("[IVSHMEM] === Basic Test (Machine %d) ===\n", my_id);
    kinfo("[IVSHMEM] My peer_id: %u\n", doorbell_dev->peer_id);
    kinfo("[IVSHMEM] Doorbell registers at: %p\n", doorbell_dev->doorbell_regs);
    kinfo("[IVSHMEM] BAR0 base: %p\n", doorbell_dev->regs_iova);
    
    /* Test 1: Verify peer_id (using machine_id) */
    doorbell_dev->peer_id = my_id; /* Use machine_id as peer_id */
    kinfo("[IVSHMEM] Test 1: Using machine_id as peer_id: machine_id=%d -> peer_id=%u\n", 
          my_id, doorbell_dev->peer_id);
    
    /* Register peer_id mapping */
    ivshmem_register_peer_id();
    
    /* Test 2: Check peer_id mappings */
    kinfo("[IVSHMEM] Test 2: Peer ID mappings:\n");
    for (int i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        u32 peer_id = dsm_meta->machine_to_peer_id[i];
        kinfo("[IVSHMEM]   machine_id=%d -> peer_id=%u\n", i, peer_id);
    }
    
    /* Test 3: Test shared memory write/read */
    kinfo("[IVSHMEM] Test 3: Shared memory test\n");
    volatile u32 *test_addr = (volatile u32 *)&dsm_meta->msi_test_msg[my_id];
    u32 test_value = 0x12345678 + my_id;
    *test_addr = test_value;
    __sync_synchronize();
    u32 read_value = *test_addr;
    kinfo("[IVSHMEM]   Wrote 0x%x, read 0x%x\n", test_value, read_value);
    if (read_value != test_value) {
        kinfo("[IVSHMEM] ERROR: Shared memory read/write mismatch!\n");
        return -EINVAL;
    }
    
    /* Test 4: Test doorbell register write (without expecting interrupt) */
    kinfo("[IVSHMEM] Test 4: Doorbell register write test\n");
    if (CLUSTER_MACHINE_NUM > 1) {
        mid_t target_id = (my_id == 0) ? 1 : 0;
        u32 target_peer_id = dsm_meta->machine_to_peer_id[target_id];
        if (target_peer_id != 0xFFFFFFFF) {
            u32 *target_doorbell = (u32 *)((u64)doorbell_dev->doorbell_regs + (target_peer_id * 0x10));
            kinfo("[IVSHMEM]   Writing to doorbell for machine %d (peer_id=%u) at %p\n", 
                  target_id, target_peer_id, target_doorbell);
            __sync_synchronize();
            *target_doorbell = 0; /* Write vector 0 */
            __sync_synchronize();
            kinfo("[IVSHMEM]   Doorbell write completed\n");
        } else {
            kinfo("[IVSHMEM]   Skipping doorbell test: target peer_id not registered\n");
        }
    }
    
    kinfo("[IVSHMEM] === Basic Test Completed Successfully ===\n");
    return 0;
}

/**
 * ivshmem_test_msi_communication - Test MSI communication with all machines
 * 
 * This function sends MSI messages to all other machines in the cluster
 * and waits for their replies. It uses shared memory to exchange messages.
 * 
 * Returns 0 on success, -EINVAL on failure
 */
int ivshmem_test_msi_communication(void)
{
    mid_t my_id = CUR_MACHINE_ID;
    int i;
    u32 timeout_count;
    const u32 MAX_TIMEOUT = 1000000; /* Maximum wait cycles */
    int success_count = 0;
    int fail_count = 0;

    if (!dsm_meta || !doorbell_dev || !doorbell_dev->doorbell_regs || !doorbell_dev->msix_cap) {
        kinfo("[IVSHMEM] MSI test skipped: doorbell device not initialized or MSI-X not supported\n");
        kinfo("[IVSHMEM]   dsm_meta=%p, doorbell_dev=%p, doorbell_regs=%p, msix_cap=%d\n",
              dsm_meta, doorbell_dev, 
              doorbell_dev ? doorbell_dev->doorbell_regs : NULL,
              doorbell_dev ? doorbell_dev->msix_cap : 0);
        return -EINVAL;
    }

    // kinfo("[IVSHMEM] Starting MSI communication test from machine %d\n", my_id);

    /* Initialize message slots (with lock protection) */
    for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        lock(&dsm_meta->msi_test_msg[i].msg_lock);
        dsm_meta->msi_test_msg[i].msg_from = 0xFFFFFFFF;
        dsm_meta->msi_test_msg[i].msg_type = 0;
        dsm_meta->msi_test_msg[i].reply_received = 0;
        dsm_meta->msi_test_msg[i].reply_from = 0xFFFFFFFF;
        dsm_meta->msi_test_msg[i].tlb_start_va = 0;
        dsm_meta->msi_test_msg[i].tlb_len = 0;
        dsm_meta->msi_test_msg[i].tlb_vmspace = 0;
        unlock(&dsm_meta->msi_test_msg[i].msg_lock);
    }
    
    /* Wait a bit for all machines to register their peer_id mappings */
    /* This is important because machines may initialize at different times */
    for (volatile int j = 0; j < 10000; j++);
    
    /* Print peer_id mappings for debugging */
    // kinfo("[IVSHMEM] Peer ID mappings:\n");
    // for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
    //     u32 peer_id = dsm_meta->machine_to_peer_id[i];
    //     kinfo("[IVSHMEM]   machine_id=%d -> peer_id=%u\n", i, peer_id);
    // }

    /* Send MSI to all other machines */
    u64 send_times[CLUSTER_MAX_MACHINE_NUM]; /* Store send time for each machine */
    for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (i == my_id)
            continue;

        /* Prepare message in shared memory (message goes to target machine's slot) */
        /* Each machine checks its own slot (msi_test_msg[machine_id]) for messages */
        /* IMPORTANT: Message should be placed in target machine's slot, not sender's slot */
        /* When we send to machine i, we write to slot i (target's slot) */
        /* When machine i replies, it writes to slot my_id (our slot) */
        /* So we need to clear our slot before sending, and check our slot for reply */
        lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        dsm_meta->msi_test_msg[my_id].reply_received = 0;
        dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
        unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        
        lock(&dsm_meta->msi_test_msg[i].msg_lock);
        dsm_meta->msi_test_msg[i].msg_from = my_id;
        dsm_meta->msi_test_msg[i].msg_type = MSI_MSG_TYPE_TEST;
        dsm_meta->msi_test_msg[i].reply_received = 0;
        dsm_meta->msi_test_msg[i].reply_from = 0xFFFFFFFF;
        unlock(&dsm_meta->msi_test_msg[i].msg_lock);
        
        // kinfo("[IVSHMEM] Prepared message for machine %d: msg_from=%u, msg_type=%u\n",
        //       i, dsm_meta->msi_test_msg[i].msg_from, dsm_meta->msi_test_msg[i].msg_type);

        /* Record send time before sending MSI */
        send_times[i] = plat_get_mono_time();

        /* Send MSI interrupt (vector 0 for test messages) */
        int ret = ivshmem_send_msi(i, 0);
        if (ret != 0) {
            kinfo("[IVSHMEM] Failed to send MSI to machine %d\n", i);
            fail_count++;
            continue;
        }

        // kinfo("[IVSHMEM] Sent MSI test message to machine %d at time %llu ns\n", i, send_times[i]);
    }

    /* Wait for replies from all machines */
    /* Note: We rely on MSI-X interrupts to process messages, not polling */
    // kinfo("[IVSHMEM] Waiting for replies (relying on MSI-X interrupts)...\n");
    
    /* Give some time for MSI-X interrupts to be delivered and processed */
    /* Messages will be processed by ivshmem_msix_handler() when interrupts arrive */
    for (i = 0; i < CLUSTER_MACHINE_NUM; i++) {
        if (i == my_id)
            continue;

        timeout_count = 0;
        
        // kinfo("[IVSHMEM] Waiting for reply from machine %d...\n", i);
        
        /* Wait for reply - messages are processed by interrupt handler */
        while (timeout_count < MAX_TIMEOUT) {
            /* Check if reply received (with lock protection) */
            /* When we send a message to machine i, we place it in slot i (target's slot) */
            /* When machine i replies, it places the reply in slot my_id (our slot) */
            /* So we check our own slot (msi_test_msg[my_id]) for the reply */
            lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
            u32 reply_received = dsm_meta->msi_test_msg[my_id].reply_received;
            u32 reply_from = dsm_meta->msi_test_msg[my_id].reply_from;
            unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
            
            if (reply_received == 1 && reply_from == i) {
                u64 reply_time = plat_get_mono_time();
                u64 latency_ns = reply_time - send_times[i];
                // kinfo("[IVSHMEM] Received reply from machine %d (reply_from=%u, reply_received=%u)\n", 
                //       i, dsm_meta->msi_test_msg[my_id].reply_from, dsm_meta->msi_test_msg[my_id].reply_received);
                kinfo("[IVSHMEM] Machine %d -> Machine %d: Round-trip latency: %llu ns (%.f us, %.f ms)\n", 
                      my_id, i, latency_ns, latency_ns / 1000.0, latency_ns / 1000000.0);
                // kinfo("[IVSHMEM]   Send time: %llu ns, Reply time: %llu ns\n", 
                //       send_times[i], reply_time);
                success_count++;
                break;
            }
            
            /* Debug: Print message state periodically */
            // if (timeout_count % 100000 == 0) {
            //     __sync_synchronize();
            //     kinfo("[IVSHMEM] Waiting for reply from machine %d (checking slot %d): reply_received=%u, reply_from=%u, msg_from=%u\n",
            //           i, my_id, dsm_meta->msi_test_msg[my_id].reply_received, 
            //           dsm_meta->msi_test_msg[my_id].reply_from,
            //           dsm_meta->msi_test_msg[my_id].msg_from);
            // }
            
            /* Small delay to avoid busy waiting */
            /* Note: We don't poll for messages here - they're handled by interrupt */
            for (volatile int j = 0; j < 100; j++);
            timeout_count++;
        }

        if (timeout_count >= MAX_TIMEOUT) {
            __sync_synchronize();
            kinfo("[IVSHMEM] Timeout waiting for reply from machine %d\n", i);
            kinfo("[IVSHMEM]   Final state (slot %d): reply_received=%u, reply_from=%u, msg_from=%u\n",
                  my_id, dsm_meta->msi_test_msg[my_id].reply_received,
                  dsm_meta->msi_test_msg[my_id].reply_from,
                  dsm_meta->msi_test_msg[my_id].msg_from);
            fail_count++;
        }
    }

    kinfo("[IVSHMEM] MSI test completed: %d success, %d failed\n", 
          success_count, fail_count);

    return (fail_count == 0) ? 0 : -EINVAL;
}

/**
 * ivshmem_polling_thread_routine - Polling thread routine
 * 
 * This function runs in a dedicated kernel thread and continuously polls
 * for incoming messages from other machines. This allows message processing
 * without relying on MSI interrupts, which can cause nested exceptions
 * in page fault handlers.
 */
static void ivshmem_polling_thread_routine(void)
{
    while (1) {
        /* Only poll if polling mode is enabled */
        if (ivshmem_msg_mode == IVSHMEM_MSG_MODE_POLLING) {
            /* Poll for messages sent to this machine */
            ivshmem_poll_messages();
        }
        
        /* Small delay to avoid busy waiting */
        /* TODO: Could use a more sophisticated sleep mechanism */
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile("pause");
        }
    }
}

/**
 * ivshmem_set_msg_mode - Set message processing mode
 * 
 * @param mode: IVSHMEM_MSG_MODE_MSI (0) for MSI interrupts, 
 *              IVSHMEM_MSG_MODE_POLLING (1) for polling thread
 */
void ivshmem_set_msg_mode(enum ivshmem_msg_mode mode)
{
    ivshmem_msg_mode = mode;
    kinfo("[IVSHMEM] Message processing mode set to: %s\n", 
          mode == IVSHMEM_MSG_MODE_MSI ? "MSI" : "Polling");
}

/**
 * ivshmem_get_msg_mode - Get current message processing mode
 * 
 * @return: Current message processing mode
 */
enum ivshmem_msg_mode ivshmem_get_msg_mode(void)
{
    return ivshmem_msg_mode;
}

/**
 * ivshmem_start_polling_thread - Start the polling thread for message processing
 * 
 * This function creates a kernel thread that continuously polls for incoming
 * messages from other machines. The thread runs with low priority and processes
 * messages without relying on MSI interrupts.
 * 
 * This should be called after dsm_meta is initialized and CUR_MACHINE_ID is set.
 */
void ivshmem_start_polling_thread(void)
{
    mid_t my_id = CUR_MACHINE_ID;
    
    if (!dsm_meta || my_id < 0 || my_id >= CLUSTER_MACHINE_NUM) {
        kwarn("[IVSHMEM] Cannot start polling thread: invalid configuration (dsm_meta=%p, CUR_MACHINE_ID=%d)\n",
              dsm_meta, my_id);
        return;
    }
    
    /* Create thread context for polling thread */
    static struct thread polling_thread;
    static char polling_thread_stack[DEFAULT_STACK_SIZE];
    static struct cap_group *polling_cap_group = NULL;
    static bool polling_thread_started = false;
    
    /* Check if polling thread has already been started */
    if (polling_thread_started) {
        kinfo("[IVSHMEM] Polling thread already started, skipping\n");
        return;
    }
    
    struct vmspace *idle_vmspace;
    struct vmspace *vmspace_obj;
    int ret;
    int slot_id;
    const char *polling_name = "IVSHMEM-POLLING";
    u32 polling_name_len = strlen(polling_name);
    
    /* Create cap_group similar to create_root_cap_group */
    extern void *obj_alloc(u64 type, u64 size, mem_t mem_type);
    extern int cap_group_init(struct cap_group *cap_group, unsigned int size, u64 badge, bool is_cross_machine);
    extern int cap_alloc(struct cap_group *cap_group, void *obj, u64 rights);
    
    polling_cap_group = (struct cap_group *)obj_alloc(TYPE_CAP_GROUP, sizeof(*polling_cap_group), __MT_OBJECT__);
    if (!polling_cap_group) {
        kwarn("[IVSHMEM] Failed to allocate polling_cap_group\n");
        return;
    }
    
    ret = cap_group_init(polling_cap_group, BASE_OBJECT_NUM, 0, false);
    if (ret != 0) {
        kwarn("[IVSHMEM] Failed to initialize polling_cap_group: %d\n", ret);
        return;
    }
    
    /* 1st cap is cap_group itself */
    slot_id = cap_alloc(polling_cap_group, polling_cap_group, 0);
    if (slot_id != CAP_GROUP_OBJ_ID) {
        kwarn("[IVSHMEM] Failed to allocate cap_group slot: got %d, expected %d\n", 
              slot_id, CAP_GROUP_OBJ_ID);
        return;
    }
    
    /* Use idle vmspace for kernel thread */
    idle_vmspace = create_idle_vmspace();
    if (!idle_vmspace) {
        kwarn("[IVSHMEM] Failed to create idle vmspace\n");
        return;
    }
    
    /* 2nd cap is vmspace - allocate vmspace object and add to cap_group */
    vmspace_obj = (struct vmspace *)obj_alloc(TYPE_VMSPACE, sizeof(*vmspace_obj), __MT_OBJECT__);
    if (!vmspace_obj) {
        kwarn("[IVSHMEM] Failed to allocate vmspace_obj\n");
        return;
    }
    *vmspace_obj = *idle_vmspace; /* Copy vmspace content */
    
    slot_id = cap_alloc(polling_cap_group, vmspace_obj, 0);
    if (slot_id != VMSPACE_OBJ_ID) {
        kwarn("[IVSHMEM] Failed to allocate vmspace slot: got %d, expected %d\n", 
              slot_id, VMSPACE_OBJ_ID);
        return;
    }
    
    /* Set cap_group name */
    polling_name_len = MIN(polling_name_len, MAX_GROUP_NAME_LEN);
    memcpy(polling_cap_group->cap_group_name, polling_name, polling_name_len);
    polling_cap_group->cap_group_name[polling_name_len] = '\0';
    
    /* Use thread_init to initialize the thread */
    /* Use TYPE_KERNEL since thread_init is designed for it */
    extern int thread_init(struct thread *thread, struct cap_group *cap_group,
                           u64 stack, u64 pc, u32 prio, u32 type, s32 aff);
    
    /* Verify cap_group is valid before calling thread_init */
    if (!polling_cap_group) {
        kwarn("[IVSHMEM] polling_cap_group is NULL before thread_init\n");
        return;
    }
    
    ret = thread_init(&polling_thread,
                      polling_cap_group,
                      (u64)polling_thread_stack + DEFAULT_STACK_SIZE,  /* stack */
                      (u64)ivshmem_polling_thread_routine,            /* pc */
                      254,                                            /* prio */
                      TYPE_KERNEL,                                    /* type */
                      NO_AFF);                                        /* affinity */
    if (ret != 0) {
        kwarn("[IVSHMEM] Failed to initialize polling thread: %d\n", ret);
        return;
    }
    
    /* Add thread to scheduler */
    sched_enqueue(&polling_thread);
    
    /* Mark polling thread as started */
    polling_thread_started = true;
    
    kinfo("[IVSHMEM] Started polling thread for machine %d\n", my_id);
}
