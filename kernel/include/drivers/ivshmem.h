/*
 * ivshmem_setup_devices: setup kvm_ivshmem_dev
 */
void ivshmem_setup_devices(void);

int pci_hostfs_open(void *req);
int pci_hostfs_mmap(void *args);
int pci_hostfs_unmap(void *args);
int pci_hostfs_list(void *args);

/* MSI-X interrupt handler for ivshmem-doorbell */
void ivshmem_msix_handler(void);

/* Register peer_id mapping in shared memory (call after dsm_meta is initialized) */
void ivshmem_register_peer_id(void);

/* Send MSI interrupt to another machine via ivshmem doorbell */
int ivshmem_send_msi(mid_t target_machine_id, u16 vector);

/* Process received MSI messages and send replies */
void ivshmem_process_msi_messages(void);

/* Basic test of doorbell and shared memory (without MSI interrupts) */
int ivshmem_test_basic(void);

/* Test MSI communication with all machines in cluster */
int ivshmem_test_msi_communication(void);
