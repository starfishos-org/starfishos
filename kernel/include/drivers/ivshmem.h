/*
 * ivshmem_setup_devices: setup kvm_ivshmem_dev
 */
void ivshmem_setup_devices(void);

int pci_hostfs_open(void *req);
int pci_hostfs_mmap(void *args);
int pci_hostfs_unmap(void *args);
int pci_hostfs_list(void *args);
