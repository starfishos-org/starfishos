/*
 * ivshmem_setup_devices: setup kvm_ivshmem_dev
 */

#include <common/types.h>

/* Message processing mode for IVSHMEM */
enum ivshmem_msg_mode {
    IVSHMEM_MSG_MODE_MSI = 0, /* Use MSI interrupts */
    IVSHMEM_MSG_MODE_POLLING = 1 /* Use user-space polling server */
};

void ivshmem_setup_devices(void);

int pci_hostfs_open(void *req);
int pci_hostfs_mmap(void *args);
int pci_hostfs_unmap(void *args);
int pci_hostfs_list(void *args);

/* MSI-X interrupt handler for ivshmem-doorbell */
void ivshmem_msix_handler(void);

/* Register peer_id mapping in shared memory (call after dsm_meta is
 * initialized) */
void ivshmem_register_peer_id(void);

/* Configure one MSI-X doorbell vector per local CPU. */
int ivshmem_configure_cpu_msix_vectors(void);

/* Send MSI interrupt to another machine via ivshmem doorbell */
int ivshmem_send_msi(mid_t target_machine_id, u16 vector);

/* Send a scheduler doorbell directly to a global CPU. */
int ivshmem_send_sched_msi(u32 target_global_cpu);

/* Measure one-way doorbell-to-remote-MSI-handler delivery latency. */
long sys_ivshmem_msi_bench(u64 target_machine, u64 target_local_cpu, u64 samples);

/* Process received MSI messages and send replies */
void ivshmem_process_msi_messages(void);

/* Basic test of doorbell and shared memory (without MSI interrupts) */
int ivshmem_test_basic(void);

/* Test MSI communication with all machines in cluster */
int ivshmem_test_msi_communication(void);

/* Set/get message processing mode */
void ivshmem_set_msg_mode(enum ivshmem_msg_mode mode);
enum ivshmem_msg_mode ivshmem_get_msg_mode(void);

/* Forward declaration for pci_dev */
struct pci_dev;

/* kvm_ivshmem_device structure definition */
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

/* Access to ivshmem device list for CXL device parsing */
extern struct kvm_ivshmem_device kvm_ivshmem_dev_list[];
extern u8 kvm_ivshmem_dev_num;
extern struct kvm_ivshmem_device *doorbell_dev;
extern struct kvm_ivshmem_device *hostfs_dev;
extern struct kvm_ivshmem_device *cxl_shm_dev;

/* Array for 8 CXL devices (numa0.0 to numa3.1) - stores physical address and size */
#define MAX_CXL_DEVICES 8
extern u64 dram_devices_map[MAX_CXL_DEVICES][2];
