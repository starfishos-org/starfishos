#include <common/types.h>

void memcpy(void *dst, const void *src, size_t size)
{
	char *d;
	char *s;
	u64 i;

	d = (char *)dst;
	s = (char *)src;
	for (i = 0; i < size; ++i)
		d[i] = s[i];
}

void memset(void *dst, const char ch, size_t size)
{
	char *d;
	u64 i;

	d = (char *)dst;
	for (i = 0; i < size; ++i)
		d[i] = ch;
}

void memmove(void *dst, const void *src, size_t size)
{
	char *d;
	char *s;
	s64 i;

	d = (char *)dst;
	s = (char *)src;
	for (i = size; i >= 0; --i)
		d[i] = s[i];
}

void bzero(void *p, size_t size)
{
	char *d;
	u64 i;

	d = (char *)p;
	for (i = 0; i < size; ++i)
		d[i] = 0;
}

void arch_flush_cache(u64 start, s64 len, int op_type)
{
	/* Not needed on x86. (at least so far) */
}

/*
 * memcpy with non-temporal hint, the processor does not write the data into
 * the cache hierarchy, nor does it fetch the corresponding cache line from
 * memory into the cache hierarchy. So clflush can be avoided.
 */
void memcpy_nt(void *dst, void *src, size_t len)
{
	int i;
	long long t1, t2, t3, t4;
	unsigned char *from, *to;
	size_t remain = len & 63;

	from = src;
	to = dst;
	i = len / 64;

	for (; i > 0; i--) {
		__asm__ __volatile__("  mov (%4), %0\n"
				     "  mov 8(%4), %1\n"
				     "  mov 16(%4), %2\n"
				     "  mov 24(%4), %3\n"
				     "  movnti %0, (%5)\n"
				     "  movnti %1, 8(%5)\n"
				     "  movnti %2, 16(%5)\n"
				     "  movnti %3, 24(%5)\n"
				     "  mov 32(%4), %0\n"
				     "  mov 40(%4), %1\n"
				     "  mov 48(%4), %2\n"
				     "  mov 56(%4), %3\n"
				     "  movnti %0, 32(%5)\n"
				     "  movnti %1, 40(%5)\n"
				     "  movnti %2, 48(%5)\n"
				     "  movnti %3, 56(%5)\n"
				     :"=r"(t1), "=r"(t2), "=r"(t3), "=r"(t4),
				     "+r"(from), "+r"(to)
				     :
				     : "memory");

		from += 64;
		to += 64;
	}

	/* Now do the tail of the block: */
	if (remain) {
		memcpy(to, from, remain);
		// arch_flush_cache(to, remain, 0);
	}

	// __asm__ __volatile__("mfence\n" : : );
}
