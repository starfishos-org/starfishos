#include <stdio.h>
#include <stdlib.h>
#include <chcore/pci_ioctl.h>
#include <chcore/syscall.h>

int main(int argc, char *argv[])
{
    int ret;
    struct pci_control_req *req;

    req = (struct pci_control_req *)malloc(sizeof(struct pci_control_req));
    req->req_type = PCI_CONTROL_LIST_DEVICES;

    ret = usys_pcie_control((u64)req);
    if (ret < 0) {
        printf("usys_pcie_control failed\n");
        return -1;
    }
    return 0;
}
