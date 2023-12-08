#pragma once

#include <common/types.h>

#define build_mmio_read(name, size, type, reg, barrier)             \
        static inline type name(const volatile void *addr)          \
        {                                                           \
                type ret;                                           \
                asm volatile("mov" size " %1,%0"                    \
                             : reg(ret)                             \
                             : "m"(*(volatile type *)addr)barrier); \
                return ret;                                         \
        }

#define build_mmio_write(name, size, type, reg, barrier)                      \
        static inline void name(type val, volatile void *addr)                \
        {                                                                     \
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
