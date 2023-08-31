#pragma once

#include <common/types.h>

void enable_cpu_cnt(void);
void disable_cpu_cnt(void);
void pmu_init(void);

static inline u64 pmu_read_real_cycle(void)
{
	u32 lo, hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return (((u64)hi) << 32) | lo;
}
