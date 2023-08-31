#pragma once

// XXX barrier

#define mb() asm volatile("mfence")
#define rmb() asm volatile("mfence")
#define wmb() asm volatile("mfence")

#define smp_mb() asm volatile("mfence")
#define smp_rmb() asm volatile("mfence")
#define smp_wmb() asm volatile("mfence")

#define CPU_PAUSE() asm volatile("pause\n":::"memory")
#define COMPILER_BARRIER() asm volatile("":::"memory")

static inline s32 compare_and_swap_32(s32 * ptr, s32 oldval, s32 newval)
{
	__asm__ __volatile__("lock cmpxchg %[newval], %[ptr]":"+a"(oldval),
			     [ptr] "+m"(*ptr)
			     :[newval] "r"(newval)
			     :"memory");
	return oldval;
}

static inline s64 compare_and_swap_64(s64 * ptr, s64 oldval, s64 newval)
{
	__asm__ __volatile__("lock cmpxchgq %[newval], %[ptr]":"+a"(oldval),
			     [ptr] "+m"(*ptr)
			     :[newval] "r"(newval)
			     :"memory");
	return oldval;
}

#define atomic_bool_compare_exchange_64(ptr, compare, exchange) \
	((compare) == compare_and_swap_64((ptr), (compare), (exchange))? 1:0)
#define atomic_bool_compare_exchange_32(ptr, compare, exchange) \
	((compare) == compare_and_swap_32((ptr), (compare), (exchange))? 1:0)

#define __atomic_xadd(ptr, val, len, width)			\
({								\
	u##len oldval = val;					\
	asm volatile ("lock xadd" #width " %0, %1"		\
		      : "+r" (oldval), "+m" (*(ptr))		\
		      : : "memory", "cc");			\
	oldval;							\
 })

#define atomic_fetch_sub_32(ptr, val) __atomic_xadd(ptr, -val, 32, l)
#define atomic_fetch_sub_64(ptr, val) __atomic_xadd(ptr, -val, 64, q)
#define atomic_fetch_add_32(ptr, val) __atomic_xadd(ptr, val, 32, l)
#define atomic_fetch_add_64(ptr, val) __atomic_xadd(ptr, val, 64, q)

static inline s64 atomic_exchange_64(void *ptr, s64 x)
{
	__asm__ __volatile__("xchgq %0,%1":"=r"((s64)x)
			     :"m"(*(s64 *)ptr),
			     "0"((s64)x)
			     :"memory");

	return x;
}
