#include "arch/mmu.h"
#include "common/types.h"
#include <mm/kmalloc.h>
#include <common/lock.h>
#include <common/list.h>
#include <common/errno.h>

#include "pci.h"

struct lock pci_mmcfg_lock;
struct list_head pci_mmcfg_list;

static struct pci_mmcfg_region *pci_mmconfig_alloc(int segment, int start,
                                                   int end, u64 addr)
{
        struct pci_mmcfg_region *new;
        struct resource *res;

        if (addr == 0)
                return NULL;

        new = (struct pci_mmcfg_region *)temp_kmalloc(sizeof(*new));
        if (!new)
                return NULL;

        new->address = addr;
        new->segment = segment;
        new->start_bus = start;
        new->end_bus = end;
        new->virt = (char *)phys_to_virt(addr);

        res = &new->res;
        res->start = addr + PCI_MMCFG_BUS_OFFSET(start);
        res->end = addr + PCI_MMCFG_BUS_OFFSET(end + 1) - 1;
        // snprintf(new->name,
        //          PCI_MMCFG_RESOURCE_NAME_LEN,
        //          "PCI MMCONFIG %04x [bus %02x-%02x]",
        //          segment,
        //          start,
        //          end);
        //
        return new;
}

struct pci_mmcfg_region *pci_mmconfig_add(int segment, int start, int end,
                                          u64 addr)
{
        struct pci_mmcfg_region *new;

        new = pci_mmconfig_alloc(segment, start, end, addr);
        if (new) {
                lock(&pci_mmcfg_lock);
                /* TODO: add sorted pci mmconfig */
                list_add(&new->list, &pci_mmcfg_list);
                unlock(&pci_mmcfg_lock);

                pci_debug("[mmcfg add] domain %04x [bus %02x-%02x] at %pR "
                          "(base %#lx)\n",
                          segment,
                          start,
                          end,
                          &new->res,
                          (unsigned long)addr);
        }

        return new;
}

struct pci_mmcfg_region *pci_mmconfig_lookup(int segment, int bus)
{
        struct pci_mmcfg_region *cfg;

        for_each_in_list (cfg, struct pci_mmcfg_region, list, &pci_mmcfg_list)
                if (cfg->segment == segment && cfg->start_bus <= bus
                    && bus <= cfg->end_bus)
                        return cfg;

        return NULL;
}

static char *pci_dev_base(unsigned int seg, unsigned int bus,
                          unsigned int devfn)
{
        struct pci_mmcfg_region *cfg = pci_mmconfig_lookup(seg, bus);

        if (cfg && cfg->virt)
                return cfg->virt + (PCI_MMCFG_BUS_OFFSET(bus) | (devfn << 12));
        return NULL;
}

static int pci_mmcfg_read(unsigned int seg, unsigned int bus,
                          unsigned int devfn, int reg, int len, u32 *value)
{
        char *addr;

        /* Why do we have this when nobody checks it. How about a BUG()!? -AK */
        if (unlikely((bus > 255) || (devfn > 255) || (reg > 4095))) {
        err:
                *value = -1;
                return -EINVAL;
        }

        addr = pci_dev_base(seg, bus, devfn);
        // pci_debug("[mmcfg read] bus=%04x devfn=%04x addr=0x%llx reg=%llx\n",
        //           bus,
        //           devfn,
        //           addr,
        //           reg);

        if (!addr) {
                goto err;
        }

        switch (len) {
        case 1:
                *value = mmio_config_readb(addr + reg);
                break;
        case 2:
                *value = mmio_config_readw(addr + reg);
                break;
        case 4:
                *value = mmio_config_readl(addr + reg);
                break;
        }

        return 0;
}

static int pci_mmcfg_write(unsigned int seg, unsigned int bus,
                           unsigned int devfn, int reg, int len, u32 value)
{
        char *addr;

        /* Why do we have this when nobody checks it. How about a BUG()!? -AK */
        if (unlikely((bus > 255) || (devfn > 255) || (reg > 4095)))
                return -EINVAL;

        addr = pci_dev_base(seg, bus, devfn);
        if (!addr) {
                return -EINVAL;
        }

        switch (len) {
        case 1:
                mmio_config_writeb(addr + reg, value);
                break;
        case 2:
                mmio_config_writew(addr + reg, value);
                break;
        case 4:
                mmio_config_writel(addr + reg, value);
                break;
        }

        return 0;
}

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
        struct pci_bus *bus = dev->bus;
        BUG_ON(!bus);
        return pci_mmcfg_read(
                bus->domain, bus->number, dev->devfn, where, 1, (u32 *)val);
}

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
        struct pci_bus *bus = dev->bus;
        BUG_ON(!bus);
        return pci_mmcfg_read(
                bus->domain, bus->number, dev->devfn, where, 2, (u32 *)val);
}

int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
        struct pci_bus *bus = dev->bus;
        BUG_ON(!bus);
        return pci_mmcfg_read(
                bus->domain, bus->number, dev->devfn, where, 4, val);
}
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
        struct pci_bus *bus = dev->bus;
        BUG_ON(!bus);
        return pci_mmcfg_write(
                bus->domain, bus->number, dev->devfn, where, 1, (u32)val);
}
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
        struct pci_bus *bus = dev->bus;
        BUG_ON(!bus);
        return pci_mmcfg_write(
                bus->domain, bus->number, dev->devfn, where, 2, (u32)val);
}
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val)
{
        struct pci_bus *bus = dev->bus;
        BUG_ON(!bus);
        return pci_mmcfg_write(
                bus->domain, bus->number, dev->devfn, where, 4, (u32)val);
}
