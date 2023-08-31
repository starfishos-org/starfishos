#pragma once

#include <chcore/type.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__aarch64__)

static inline u64 pmu_read_real_cycle(void)
{
	s64 tv;
	asm volatile("mrs %0, pmccntr_el0" : "=r"(tv));
	return tv;
}

static inline void pmu_clear_cnt(void)
{
	asm volatile("msr pmccntr_el0, %0" ::"r"(0));
}

#endif

#if defined(__x86_64__)

static inline u64 pmu_read_real_cycle(void)
{
	u32 lo, hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return (((u64)hi) << 32) | lo;
}

static void pmu_clear_cnt(void)
{
	/* No such function */
}

#endif

#if defined(__riscv)

static inline u64 pmu_read_real_cycle(void)
{
	u64 cycle;
	asm volatile ("rdcycle %0" : "=r" (cycle));
	return cycle;
}

static inline void pmu_clear_cnt(void)
{
	/* No such function */
}

#endif

#ifdef __cplusplus
}
#endif
