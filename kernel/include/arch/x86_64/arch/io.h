#pragma once

#include <drivers/pci.h>
#include <common/types.h>

#define build_mmio_read(name, size, type, reg, barrier)     \
    static inline type name(const volatile void *addr)      \
    {                                                       \
        type ret;                                           \
        asm volatile("mov" size " %1,%0"                    \
                     : reg(ret)                             \
                     : "m"(*(volatile type *)addr)barrier); \
        return ret;                                         \
    }

#define build_mmio_write(name, size, type, reg, barrier)              \
    static inline void name(type val, volatile void *addr)            \
    {                                                                 \
        asm volatile("mov" size " %0,%1"                              \
                     :                                                \
                     : reg(val), "m"(*(volatile type *)addr)barrier); \
    }

build_mmio_read(readb, "b", unsigned char, "=q",
                : "memory") build_mmio_read(readw, "w", unsigned short, "=r",
                                            : "memory")
        build_mmio_read(readl, "l", unsigned int, "=r",
                        : "memory") build_mmio_read(readq, "q", unsigned long,
                                                    "=r",
                                                    : "memory")

                build_mmio_write(writeb, "b", unsigned char, "q",
                                 : "memory")
                        build_mmio_write(writew, "w", unsigned short, "r",
                                         : "memory")
                                build_mmio_write(writel, "l", unsigned int, "r",
                                                 : "memory")

                                        static inline u8 get8(u16 port)
{
    u8 data = 0;
    __asm__ __volatile__("inb %1, %0" : "=a"(data) : "d"(port));
    return data;
}

static inline void put8(u16 port, u8 data)
{
    __asm__ __volatile__("outb %0, %1" : : "a"(data), "d"(port));
}

static inline u32 get32(u16 port)
{
    u32 data = 0;
    __asm__ __volatile__("inl %1, %0" : "=a"(data) : "d"(port));
    return data;
}

static inline void put32(u16 port, u32 data)
{
    __asm__ __volatile__("outl %0, %1" : : "a"(data), "d"(port));
}

static inline void pause(void)
{
    __asm__ __volatile__("pause" ::);
}

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
