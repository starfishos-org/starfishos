#include <arch/mmu.h>
#include <common/kprint.h>
#include <mm/kmalloc.h>
#include <drivers/cxl.h>
#include <drivers/pci.h>
#include <common/errno.h>
#include <common/bitfield.h>
#include <drivers/cxl.h>
#include <drivers/cxl-pci.h>
#include <drivers/pci-special.h>

static struct cxl_memdev_state *cxl_memdev_state_create(struct pci_dev *pdev)
{
        struct cxl_memdev_state *mds = temp_kmalloc(sizeof(*mds));
        mds->cxlds = temp_kmalloc(sizeof(*(mds->cxlds)));
        return mds;
}

static bool cxl_decode_regblock(struct pci_dev *pdev, u32 reg_lo, u32 reg_hi,
                                struct cxl_register_map *map)
{
        int bar = FIELD_GET(CXL_DVSEC_REG_LOCATOR_BIR_MASK, reg_lo);
        u64 offset = ((u64)reg_hi << 32)
                     | (reg_lo & CXL_DVSEC_REG_LOCATOR_BLOCK_OFF_LOW_MASK);

        if (offset > pci_resource_len(pdev, bar)) {
                cxl_error("BAR%d: %pr: too small (offset: %pa, type: %d)\n",
                          bar,
                          &pdev->resource[bar],
                          &offset,
                          map->reg_type);
                return false;
        }

        map->reg_type = FIELD_GET(CXL_DVSEC_REG_LOCATOR_BLOCK_ID_MASK, reg_lo);
        map->resource = pci_resource_start(pdev, bar) + offset;
        map->max_size = pci_resource_len(pdev, bar) - offset;

        cxl_debug(
                "[CXL] [REGBLK] bar: %d  map reg_type: %d, resource: %llx, size: %llx\n",
                bar,
                map->reg_type,
                map->resource,
                map->max_size);

        return true;
}

/**
 * cxl_find_regblock_instance() - Locate a register block by type / index
 * @pdev: The CXL PCI device to enumerate.
 * @type: Register Block Indicator id
 * @map: Enumeration output, clobbered on error
 * @index: Index into which particular instance of a regblock wanted in the
 *	   order found in register locator DVSEC.
 *
 * Return: 0 if register block enumerated, negative error code otherwise
 *
 * A CXL DVSEC may point to one or more register blocks, search for them
 * by @type and @index.
 */
int cxl_find_regblock_instance(struct pci_dev *pdev, enum cxl_regloc_type type,
                               struct cxl_register_map *map, int index)
{
        u32 regloc_size, regblocks;
        int instance = 0;
        int regloc, i;

        *map = (struct cxl_register_map){
                // .dev = &pdev->dev,
                .resource = CXL_RESOURCE_NONE,
        };

        regloc = pci_find_dvsec_capability(
                pdev, PCI_DVSEC_VENDOR_ID_CXL, CXL_DVSEC_REG_LOCATOR);
        if (!regloc)
                return -ENXIO;

        pci_read_config_dword(pdev, regloc + PCI_DVSEC_HEADER1, &regloc_size);
        regloc_size = FIELD_GET(PCI_DVSEC_HEADER1_LENGTH_MASK, regloc_size);

        regloc += CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET;
        regblocks = (regloc_size - CXL_DVSEC_REG_LOCATOR_BLOCK1_OFFSET) / 8;

        for (i = 0; i < regblocks; i++, regloc += 8) {
                u32 reg_lo, reg_hi;

                pci_read_config_dword(pdev, regloc, &reg_lo);
                pci_read_config_dword(pdev, regloc + 4, &reg_hi);

                if (!cxl_decode_regblock(pdev, reg_lo, reg_hi, map))
                        continue;

                if (map->reg_type == type) {
                        if (index == instance)
                                return 0;
                        instance++;
                }
        }

        map->resource = CXL_RESOURCE_NONE;
        return -ENODEV;
}

/**
 * cxl_find_regblock() - Locate register blocks by type
 * @pdev: The CXL PCI device to enumerate.
 * @type: Register Block Indicator id
 * @map: Enumeration output, clobbered on error
 *
 * Return: 0 if register block enumerated, negative error code otherwise
 *
 * A CXL DVSEC may point to one or more register blocks, search for them
 * by @type.
 */
int cxl_find_regblock(struct pci_dev *pdev, enum cxl_regloc_type type,
                      struct cxl_register_map *map)
{
        return cxl_find_regblock_instance(pdev, type, map, 0);
}

static int cxl_probe_regs(struct cxl_register_map *map)
{
        struct cxl_component_reg_map *comp_map;
        struct cxl_device_reg_map *dev_map;
        struct pci_dev *pdev = map->pdev;
        void *base = map->base;

        switch (map->reg_type) {
        case CXL_REGLOC_RBI_COMPONENT:
                comp_map = &map->component_map;
                cxl_debug("[CXL] BI_COMPONENT base: 0x%llx\n", base);
                cxl_probe_component_regs(pdev, base, comp_map);
                cxl_debug("[CXL] Set up component registers\n");
                break;
        case CXL_REGLOC_RBI_MEMDEV:
                dev_map = &map->device_map;
                cxl_debug("[CXL] RBI_MEMDEV base: 0x%llx\n", base);
                cxl_probe_device_regs(pdev, base, dev_map);
                if (!dev_map->status.valid || !dev_map->mbox.valid
                    || !dev_map->memdev.valid) {
                        cxl_error("[CXL] registers not found: %s%s%s\n",
                                  !dev_map->status.valid ? "status " : "",
                                  !dev_map->mbox.valid ? "mbox " : "",
                                  !dev_map->memdev.valid ? "memdev " : "");
                        return -ENXIO;
                }
                break;
        default:
                break;
        }

        return 0;
}

static int cxl_map_regblock(struct cxl_register_map *map)
{
        // map->base = ioremap(map->resource, map->max_size);
        map->base = (void *)phys_to_virt(map->resource);
        if (!map->base) {
                cxl_error("[CXL] Failed to map registers\n");
                return -ENOMEM;
        }

        cxl_debug("[CXL] Mapped CXL Memory Device resource %pa\n",
                  &map->resource);
        return 0;
}

static void cxl_unmap_regblock(struct cxl_register_map *map)
{
        // iounmap(map->base);
        map->base = NULL;
}

static int cxl_setup_regs(struct pci_dev *pdev, enum cxl_regloc_type type,
                          struct cxl_register_map *map)
{
        int rc;

        rc = cxl_find_regblock(pdev, type, map);
        if (rc)
                return rc;

        rc = cxl_map_regblock(map);
        if (rc)
                return rc;

        rc = cxl_probe_regs(map);
        cxl_unmap_regblock(map);

        return rc;
}

static int cxl_pci_probe(struct pci_dev *pdev)
{
        struct cxl_dev_state *cxlds;
        struct cxl_memdev_state *mds;
        struct cxl_register_map map;
        int rc;

        mds = cxl_memdev_state_create(pdev);
        if (!mds)
                cxl_error("[CXL] create cxl memdev state failed\n", __func__);
        cxlds = mds->cxlds;

        cxlds->cxl_dvsec = pci_find_dvsec_capability(
                pdev, PCI_DVSEC_VENDOR_ID_CXL, CXL_DVSEC_PCIE_DEVICE);
        if (!cxlds->cxl_dvsec) {
                cxl_error("[CXL] Device DVSEC not present\n");
        }

        rc = cxl_setup_regs(pdev, CXL_REGLOC_RBI_MEMDEV, &map);
        if (rc)
                return rc;
        /*
         * If the component registers can't be found, the cxl_pci driver may
         * still be useful for management functions so don't return an error.
         */
        rc = cxl_setup_regs(pdev, CXL_REGLOC_RBI_COMPONENT, &map);
        if (rc)
                cxl_error("No component registers (%d)\n", rc);

        return 0;
}

void cxl_setup_dev(struct pci_dev *pdev)
{
        if (pdev->vendor == PCI_VENDOR_ID_INTEL) {
                switch (pdev->device) {
                case 0x7075:
                        cxl_info("find Root Port\n");
                        // cxl_pci_probe(pdev);
                        break;
                case 0xd93:
                        cxl_info("find Type3 device\n");
                        cxl_pci_probe(pdev);
                        break;
                default:
                        break;
                }
        }
}

void cxl_setup_devices()
{
        /* probe all cxl devices and set hdm decoder */
        pci_buses_traverse_all(cxl_setup_dev);
}
