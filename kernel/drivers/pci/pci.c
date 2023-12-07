#include "common/kprint.h"
#include <drivers/pci.h>

/**
 * pci_find_next_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @start: address at which to start looking (0 to start at beginning of list)
 * @cap: capability code
 *
 * Returns the address of the next matching extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Some capabilities can occur several times, e.g., the
 * vendor-specific capability, and this provides a way to find them all.
 */
u16 pci_find_next_ext_capability(struct pci_dev *dev, u16 start, int cap)
{
        u32 header;
        int ttl;
        u16 pos = PCI_CFG_SPACE_SIZE;

        /* minimum 8 bytes per capability */
        ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;

        // if (dev->cfg_size <= PCI_CFG_SPACE_SIZE)
        // 	return 0;

        if (start)
                pos = start;

        if (pci_read_config_dword(dev, pos, &header) != 0)
                return 0;
        /*
         * If we have no capabilities, this is indicated by cap ID,
         * cap version and next pointer all being 0.
         */
        if (header == 0)
                return 0;

        while (ttl-- > 0) {
                if (PCI_EXT_CAP_ID(header) == cap && pos != start)
                        return pos;

                pos = PCI_EXT_CAP_NEXT(header);
                if (pos < PCI_CFG_SPACE_SIZE)
                        break;

                if (pci_read_config_dword(dev, pos, &header) != 0)
                        break;
        }

        return 0;
}

/**
 * pci_find_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Returns the address of the requested extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Possible values for @cap include:
 *
 *  %PCI_EXT_CAP_ID_ERR		Advanced Error Reporting
 *  %PCI_EXT_CAP_ID_VC		Virtual Channel
 *  %PCI_EXT_CAP_ID_DSN		Device Serial Number
 *  %PCI_EXT_CAP_ID_PWR		Power Budgeting
 */
u16 pci_find_ext_capability(struct pci_dev *dev, int cap)
{
        return pci_find_next_ext_capability(dev, 0, cap);
}

/**
 * pci_find_dvsec_capability - Find DVSEC for vendor
 * @dev: PCI device to query
 * @vendor: Vendor ID to match for the DVSEC
 * @dvsec: Designated Vendor-specific capability ID
 *
 * If DVSEC has Vendor ID @vendor and DVSEC ID @dvsec return the capability
 * offset in config space; otherwise return 0.
 */
u16 pci_find_dvsec_capability(struct pci_dev *dev, u16 vendor, u16 dvsec)
{
        int pos;

        pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DVSEC);
        if (!pos)
                return 0;

        while (pos) {
                u16 v, id;

                pci_read_config_word(dev, pos + PCI_DVSEC_HEADER1, &v);
                pci_read_config_word(dev, pos + PCI_DVSEC_HEADER2, &id);

                if (vendor == v && dvsec == id)
                        return pos;

                pos = pci_find_next_ext_capability(
                        dev, pos, PCI_EXT_CAP_ID_DVSEC);
        }

        return 0;
}

static u8 pci_hdr_type(struct pci_dev *dev)
{
        u8 hdr_type;

        pci_read_config_byte(dev, PCI_HEADER_TYPE, &hdr_type);
        return hdr_type;
}

int pci_cfg_space_size(struct pci_dev *dev)
{
        kwarn("%s: need to check if it is a PCIE device\n", __func__);
        return PCI_CFG_SPACE_EXP_SIZE;
}
/**
 * pci_setup_device - Fill in class and map information of a device
 * @dev: the device structure to fill
 *
 * Initialize the device structure with information about the device's
 * vendor,class,memory and IO-space addresses, IRQ lines etc.
 * Called at initialisation of the PCI subsystem and by CardBus services.
 * Returns 0 on success and negative if unknown type of device (not normal,
 * bridge or CardBus).
 */
int pci_setup_device(struct pci_dev *dev)
{
        u8 hdr_type;

        hdr_type = pci_hdr_type(dev);

        dev->hdr_type = hdr_type & 0x7f;

        /* Need to have dev->class ready */
        dev->cfg_size = pci_cfg_space_size(dev);

        kinfo("[%04x:%04x] type %02x class %#08x\n",
              dev->vendor,
              dev->device,
              dev->hdr_type,
              dev->class);

        switch (dev->hdr_type) { /* header type */
        case PCI_HEADER_TYPE_NORMAL: /* standard header */
                break;
        case PCI_HEADER_TYPE_BRIDGE: /* bridge header */
                /*
                 * The PCI-to-PCI bridge spec requires that subtractive
                 * decoding (i.e. transparent) bridge must have programming
                 * interface code of 0x01.
                 */
                break;
        case PCI_HEADER_TYPE_CARDBUS: /* CardBus bridge header */
                break;

        default: /* unknown header */
                dev->class = 0;
        }

        /* We found a fine healthy device, go go go... */
        return 0;
}

/* PCI setup all valid devices */
void pci_setup_devices()
{
        /* loop mmcfg and setup valid device */
        kinfo("%s: TODO\n", __func__);
        arch_pci_probe_devices();
}

/* PCI report devices */
void pci_device_report()
{
        /* loop mmcfg and setup valid device */
        kinfo("%s: TODO\n", __func__);
}
