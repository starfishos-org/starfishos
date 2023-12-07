#include "drivers/pci.h"
#include <common/types.h>

/*
 * On AMD Fam10h CPUs, all PCI MMIO configuration space accesses must use
 * %eax.  No other source or target registers may be used.  The following
 * mmio_config_* accessors enforce this.  See "BIOS and Kernel Developer's
 * Guide (BKDG) For AMD Family 10h Processors", rev. 3.48, sec 2.11.1,
 * "MMIO Configuration Coding Requirements".
 */
static inline unsigned char mmio_config_readb(void *pos)
{
        u8 val;
        asm volatile("movb (%1),%%al" : "=a"(val) : "r"(pos));
        return val;
}

static inline unsigned short mmio_config_readw(void *pos)
{
        u16 val;
        asm volatile("movw (%1),%%ax" : "=a"(val) : "r"(pos));
        return val;
}

static inline unsigned int mmio_config_readl(void *pos)
{
        u32 val;
        asm volatile("movl (%1),%%eax" : "=a"(val) : "r"(pos));
        return val;
}

static inline void mmio_config_writeb(void *pos, u8 val)
{
        asm volatile("movb %%al,(%1)" : : "a"(val), "r"(pos) : "memory");
}

static inline void mmio_config_writew(void *pos, u16 val)
{
        asm volatile("movw %%ax,(%1)" : : "a"(val), "r"(pos) : "memory");
}

static inline void mmio_config_writel(void *pos, u32 val)
{
        asm volatile("movl %%eax,(%1)" : : "a"(val), "r"(pos) : "memory");
}

static inline void pci_config_readb(void *cfg, u32 off, u8 *val)
{
        *val = mmio_config_readb(cfg + off);
}

static inline void pci_config_readw(void *cfg, u32 off, u16 *val)
{
        *val = mmio_config_readw(cfg + off);
}

static inline void pci_config_readl(void *cfg, u32 off, u32 *val)
{
        *val = mmio_config_readl(cfg + off);
}

#define PCI_MMCFG_BUS_OFFSET(bus) ((bus) << 20)
