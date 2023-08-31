#ifndef CHCORE_DEBUG_LOCK
#define CHCORE_DEBUG_LOCK

static inline void chcore_spin_lock(volatile int *lk)
{
	for (;;) {
		if (!__atomic_test_and_set(lk, __ATOMIC_ACQUIRE))
			break;
		while (*lk)
			__asm__ __volatile__("pause\n" ::: "memory");
	}
}

static inline void chcore_spin_unlock(volatile int *lk)
{
	__asm__ __volatile__("nop" ::: "memory");
	*lk = 0;
}

#endif
