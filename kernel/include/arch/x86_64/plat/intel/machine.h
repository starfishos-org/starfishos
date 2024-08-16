#pragma once

/* Should be set at boot time */
#define PLAT_CPU_NUM (64)

/* MSR Registers */
#define IA32_APIC_BASE		0x0000001b
#define MSR_IA32_TSC_DEADLINE	0x000006e0
#define MSR_STAR		0xc0000081
#define MSR_LSTAR		0xc0000082
#define MSR_CSTAR		0xc0000083
#define MSR_SFMASK		0xc0000084
/* IA32_KERNEL_GS_BASE, will be swap to IA32_GS_BASE after swapgs */
#define MSR_GSBASE		0xc0000102

/* BITs in IA32_APIC_BASE */
#define APIC_BASE_X2APEC_ENABLE   (1 << 10)

#ifndef __ASM__
#include <common/types.h>

static inline u64 rdmsr(u64 msr)
{
	u32 high, low;

	__asm__ __volatile__("rdmsr":"=d"(high), "=a"(low):"c"(msr));
	return ((u64) low) | (((u64) high) << 32);
}

static inline void wrmsr(u64 msr, u64 val)
{
	u32 high, low;

	low = val & 0xffffffff;	/* low 32-bit */
	high = val >> 32;
	__asm__ __volatile__("wrmsr"::"c"(msr), "a"(low), "d"(high):"memory");
}

/* cpuid definations */
#define CPUID_FEATURE		1

#define FEATURE_ECX_X2APIC	(1 << 21)
#define FEATURE_EDX_XAPIC	(1 << 9)

static inline void cpuid(u32 op, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
	*eax = op;
	*ecx = 0;
	asm volatile("cpuid"
		     : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
		     : "a" (*eax), "c" (*ecx)
		     : "memory");
}
#endif
