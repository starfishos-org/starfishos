#pragma once

#include <common/types.h>

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
	__asm__ __volatile__("pause"::);
}
