#pragma once

#include <lib/printk.h>
#include <common/types.h>
#include <common/macro.h>

/* Defined in x2apic.h */
extern u64 cur_freq;

static inline u64 get_cycles(void)
{
	u32 lo, hi;

	asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (((u64)hi) << 32) | lo;
}

static inline void delay_ms(u32 ms)
{
	BUG_ON(cur_freq == 0);
	u64 tscdelay = (cur_freq * ms) / 1000;
	u64 s = get_cycles();

	while (get_cycles() - s < tscdelay)
		asm volatile("nop");
}

u64 get_tsc_through_pit();
