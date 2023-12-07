#include <common/types.h>

#define PCI_VENDOR_ID_INTEL 0x8086

#define PCI_DEVICE_ID_INTEL_E7520_MCH 0x3590
#define PCI_DEVICE_ID_INTEL_82945G_HB 0x2770

#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))

#if 0
struct pci_mmcfg_hostbridge_probe {
        u32 bus;
        u32 devfn;
        u32 vendor;
        u32 device;
        const char *(*probe)(void);
};

static const struct pci_mmcfg_hostbridge_probe pci_mmcfg_probes[] __initconst = {
        {0,
         PCI_DEVFN(0, 0),
         PCI_VENDOR_ID_INTEL,
         PCI_DEVICE_ID_INTEL_E7520_MCH,
         pci_mmcfg_e7520},
        {0,
         PCI_DEVFN(0, 0),
         PCI_VENDOR_ID_INTEL,
         PCI_DEVICE_ID_INTEL_82945G_HB,
         pci_mmcfg_intel_945},
};
#endif
