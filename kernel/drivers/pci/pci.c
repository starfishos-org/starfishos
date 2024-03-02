#include <common/kprint.h>
#include <drivers/pci-regs.h>
#include <drivers/pci-special.h>
#include <drivers/pci.h>
#include <drivers/io.h>
#include <drivers/cxl.h>
#include <drivers/ivshmem.h>

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
        kwarn_once("%s: need to check if it is a PCIE device\n", __func__);
        if (dev->vendor == PCI_VENDOR_ID_IVSHMEM
            && dev->device == PCI_DEVICE_ID_QEMU_IVSHMEM)
                return PCI_CFG_SPACE_SIZE;
        return PCI_CFG_SPACE_EXP_SIZE;
}

static u32 pci_class(struct pci_dev *dev)
{
        u32 class;

        pci_read_config_dword(dev, PCI_CLASS_REVISION, &class);
        return class;
}

static inline unsigned long decode_bar(struct pci_dev *dev, u32 bar)
{
        u32 mem_type;
        unsigned long flags;

        if ((bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_IO) {
                flags = bar & ~PCI_BASE_ADDRESS_IO_MASK;
                flags |= IORESOURCE_IO;
                return flags;
        }

        flags = bar & ~PCI_BASE_ADDRESS_MEM_MASK;
        flags |= IORESOURCE_MEM;
        if (flags & PCI_BASE_ADDRESS_MEM_PREFETCH)
                flags |= IORESOURCE_PREFETCH;

        mem_type = bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK;
        switch (mem_type) {
        case PCI_BASE_ADDRESS_MEM_TYPE_32:
                break;
        case PCI_BASE_ADDRESS_MEM_TYPE_1M:
                /* 1M mem BAR treated as 32-bit BAR */
                break;
        case PCI_BASE_ADDRESS_MEM_TYPE_64:
                flags |= IORESOURCE_MEM_64;
                break;
        default:
                /* mem unknown type treated as 32-bit BAR */
                break;
        }
        return flags;
}

/*
 * Reading from a device that doesn't respond typically returns ~0.  A
 * successful read from a device may also return ~0, so you need additional
 * information to reliably identify errors.
 */
#define PCI_ERROR_RESPONSE (~0ULL)
#define PCI_SET_ERROR_RESPONSE(val) \
        (*(val) = ((typeof(*(val)))PCI_ERROR_RESPONSE))
#define PCI_POSSIBLE_ERROR(val) ((val) == ((typeof(val))PCI_ERROR_RESPONSE))

#define PCI_COMMAND_DECODE_ENABLE (PCI_COMMAND_MEMORY | PCI_COMMAND_IO)

static u64 pci_size(u64 base, u64 maxbase, u64 mask)
{
        u64 size = mask & maxbase; /* Find the significant bits */
        if (!size)
                return 0;

        /*
         * Get the lowest of them to find the decode size, and from that
         * the extent.
         */
        size = size & ~(size - 1);

        /*
         * base == maxbase can be valid only if the BAR has already been
         * programmed with all 1s.
         */
        if (base == maxbase && ((base | (size - 1)) & mask) != mask)
                return 0;

        return size;
}

/**
 * __pci_read_base - Read a PCI BAR
 * @dev: the PCI device
 * @type: type of the BAR
 * @res: resource buffer to be filled in
 * @pos: BAR position in the config space
 *
 * Returns 1 if the BAR is 64-bit, or 0 if 32-bit.
 */
int __pci_read_base(struct pci_dev *dev, enum pci_bar_type type,
                    struct resource *res, unsigned int pos)
{
        u32 l = 0, sz = 0, mask;
        u64 l64, sz64, mask64;
        u16 orig_cmd;
        // struct pci_bus_region region; //, inverted_region;

        mask = type ? PCI_ROM_ADDRESS_MASK : ~0;

        /* No printks while decoding is disabled! */
        if (!dev->mmio_always_on) {
                pci_read_config_word(dev, PCI_COMMAND, &orig_cmd);
                if (orig_cmd & PCI_COMMAND_DECODE_ENABLE) {
                        pci_write_config_word(
                                dev,
                                PCI_COMMAND,
                                orig_cmd & ~PCI_COMMAND_DECODE_ENABLE);
                }
        }

        // res->name = pci_name(dev);

        pci_read_config_dword(dev, pos, &l);
        pci_write_config_dword(dev, pos, l | mask);
        pci_read_config_dword(dev, pos, &sz);
        pci_write_config_dword(dev, pos, l);

        /*
         * All bits set in sz means the device isn't working properly.
         * If the BAR isn't implemented, all bits must be 0.  If it's a
         * memory BAR or a ROM, bit 0 must be clear; if it's an io BAR, bit
         * 1 must be clear.
         */
        if (PCI_POSSIBLE_ERROR(sz))
                sz = 0;

        /*
         * I don't know how l can have all bits set.  Copied from old code.
         * Maybe it fixes a bug on some ancient platform.
         */
        if (PCI_POSSIBLE_ERROR(l))
                l = 0;

        if (type == pci_bar_unknown) {
                res->flags = decode_bar(dev, l);
                res->flags |= IORESOURCE_SIZEALIGN;
                if (res->flags & IORESOURCE_IO) {
                        l64 = l & PCI_BASE_ADDRESS_IO_MASK;
                        sz64 = sz & PCI_BASE_ADDRESS_IO_MASK;
                        mask64 = PCI_BASE_ADDRESS_IO_MASK & (u32)IO_SPACE_LIMIT;
                } else {
                        l64 = l & PCI_BASE_ADDRESS_MEM_MASK;
                        sz64 = sz & PCI_BASE_ADDRESS_MEM_MASK;
                        mask64 = (u32)PCI_BASE_ADDRESS_MEM_MASK;
                }
        } else {
                if (l & PCI_ROM_ADDRESS_ENABLE)
                        res->flags |= IORESOURCE_ROM_ENABLE;
                l64 = l & PCI_ROM_ADDRESS_MASK;
                sz64 = sz & PCI_ROM_ADDRESS_MASK;
                mask64 = PCI_ROM_ADDRESS_MASK;
        }

        if (res->flags & IORESOURCE_MEM_64) {
                pci_read_config_dword(dev, pos + 4, &l);
                pci_write_config_dword(dev, pos + 4, ~0);
                pci_read_config_dword(dev, pos + 4, &sz);
                pci_write_config_dword(dev, pos + 4, l);

                l64 |= ((u64)l << 32);
                sz64 |= ((u64)sz << 32);
                mask64 |= ((u64)~0 << 32);
        }

        if (!dev->mmio_always_on && (orig_cmd & PCI_COMMAND_DECODE_ENABLE))
                pci_write_config_word(dev, PCI_COMMAND, orig_cmd);

        if (!sz64)
                goto fail;

        sz64 = pci_size(l64, sz64, mask64);
        if (!sz64) {
                pci_debug("reg 0x%x: invalid BAR (can't size)\n", pos);
                goto fail;
        }
#if 0
        if (res->flags & IORESOURCE_MEM_64) {
                if ((sizeof(pci_bus_addr_t) < 8 || sizeof(resource_size_t) < 8)
                    && sz64 > 0x100000000ULL) {
                        res->flags |= IORESOURCE_UNSET | IORESOURCE_DISABLED;
                        res->start = 0;
                        res->end = 0;
                        kwarn("reg 0x%x: can't handle BAR larger than 4GB (size %#010llx)\n",
                              pos,
                              (unsigned long long)sz64);
                        goto out;
                }

                if ((sizeof(pci_bus_addr_t) < 8) && l) {
                        /* Above 32-bit boundary; try to reallocate */
                        res->flags |= IORESOURCE_UNSET;
                        res->start = 0;
                        res->end = sz64 - 1;
                        pci_debug(
                                "reg 0x%x: can't handle BAR above 4GB (bus address %#010llx)\n",
                                pos,
                                (unsigned long long)l64);
                        goto out;
                }
        }
#endif
        // region.start = l64;
        // region.end = l64 + sz64 - 1;
        // pcibios_bus_to_resource(dev->bus, res, &region);
        // pcibios_resource_to_bus(dev->bus, &inverted_region, res);
        res->start = l64;
        res->end = l64 + sz64 - 1;

        pci_debug("resource flags: %x range: %lx - %lx\n",
                  res->flags,
                  res->start,
                  res->end);

#if 0
        /*
         * If "A" is a BAR value (a bus address), "bus_to_resource(A)" is
         * the corresponding resource address (the physical address used by
         * the CPU.  Converting that resource address back to a bus address
         * should yield the original BAR value:
         *
         *     resource_to_bus(bus_to_resource(A)) == A
         *
         * If it doesn't, CPU accesses to "bus_to_resource(A)" will not
         * be claimed by the device.
         */
        if (inverted_region.start != region.start) {
                res->flags |= IORESOURCE_UNSET;
                res->start = 0;
                res->end = region.end - region.start;
                pci_debug("reg 0x%x: initial BAR value %#010llx invalid\n",
                      pos,
                      (unsigned long long)region.start);
        }
#endif
        goto out;

fail:
        res->flags = 0;
out:
        if (res->flags)
                kdebug("reg 0x%x: %pR\n", pos, res);

        return (res->flags & IORESOURCE_MEM_64) ? 1 : 0;
}

static void pci_read_bases(struct pci_dev *dev, unsigned int howmany, int rom)
{
        unsigned int pos, reg;

        for (pos = 0; pos < howmany; pos++) {
                struct resource *res = &dev->resource[pos];
                reg = PCI_BASE_ADDRESS_0 + (pos << 2);
                pos += __pci_read_base(dev, pci_bar_unknown, res, reg);
        }

        if (rom) {
                struct resource *res = &dev->resource[PCI_ROM_RESOURCE];
                dev->rom_base_reg = rom;
                res->flags = IORESOURCE_MEM | IORESOURCE_PREFETCH
                             | IORESOURCE_READONLY | IORESOURCE_SIZEALIGN;
                __pci_read_base(dev, pci_bar_mem32, res, rom);
        }
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
        u32 class;
        u8 hdr_type;

        hdr_type = pci_hdr_type(dev);
        dev->hdr_type = hdr_type & 0x7f;

        class = pci_class(dev);
        dev->revision = class & 0xff;
        dev->class = class >> 8; /* upper 3 bytes */

        /* Need to have dev->class ready */
        dev->cfg_size = pci_cfg_space_size(dev);

        pci_debug("[pci setup device] [%04x:%04x] type %02x class %08x\n",
                  dev->vendor,
                  dev->device,
                  dev->hdr_type,
                  dev->class);

        switch (dev->hdr_type) { /* header type */
        case PCI_HEADER_TYPE_NORMAL: /* standard header */
                // pci_read_irq(dev);
                pci_read_bases(dev, 6, PCI_ROM_ADDRESS);
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
        arch_pci_mmcfg_init();
        /* loop mmcfg and setup valid device */
        // pci_debug("%s: TODO\n", __func__);
        arch_pci_probe_devices();

        cxl_setup_devices();
        ivshmem_setup_devices();
}
