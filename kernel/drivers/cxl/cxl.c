#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <drivers/cxl.h>
#include <drivers/pci.h>

#include <drivers/cxl-pci.h>

static struct cxl_memdev_state *cxl_memdev_state_create(struct pci_dev *pdev)
{
        struct cxl_memdev_state *mds = kzalloc(sizeof(*mds));
        mds->cxlds = kzalloc(sizeof(*(mds->cxlds)));
        return mds;
}

static void cxl_pci_probe(struct pci_dev *pdev)
{
        struct cxl_dev_state *cxlds;
        struct cxl_memdev_state *mds;

        mds = cxl_memdev_state_create(pdev);
        if (!mds)
                kwarn("%s: create cxl memdev state failed\n", __func__);
        cxlds = mds->cxlds;

        cxlds->cxl_dvsec = pci_find_dvsec_capability(
                pdev, PCI_DVSEC_VENDOR_ID_CXL, CXL_DVSEC_PCIE_DEVICE);
        if (cxlds->cxl_dvsec) {
                kinfo("[CXL PCI] find DVSEC of cxl device\n");
        }
}

void cxl_setup_devices()
{
        kinfo("%s: TODO\n", __func__);
        pci_buses_traverse_all(cxl_pci_probe);
}
