/*
 * ivshmem_setup_devices: setup kvm_ivshmem_dev
 */
void ivshmem_setup_devices(void);

int pci_ivshmem_open(void *req);
int pci_ivshmem_close(void *args);
