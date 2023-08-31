#ifndef CHCORE_DEBUG_LOCK
#define CHCORE_DEBUG_LOCK

static inline void chcore_spin_lock(volatile int *lk)
{
	for (;;) {
		if (!__atomic_test_and_set(lk, __ATOMIC_ACQUIRE))
			break;
		while (*lk)
			__asm__ __volatile__("nop" ::: "memory");
			/* TODO Set HCR_EL2.{TWE, TWI} to 0 to allow enter low power mode in user space */
			// __asm__ __volatile__("wfe" ::: "memory");
	}
}

static inline void chcore_spin_unlock(volatile int *lk)
{
	__asm__ __volatile__("dmb ish" ::: "memory");
	*lk = 0;
	/* TODO Set HCR_EL2.{TWE, TWI} to 0 to allow enter low power mode in user space */
	// __asm__ __volatile__("sev" ::: "memory");
}

#endif
