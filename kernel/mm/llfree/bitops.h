#pragma once

#include <common/types.h>

static inline ll_unused size_t trailing_zeros(uint64_t x)
{
	if (x == 0)
		return 64;
	size_t n = 0;
	while ((x & 1ULL) == 0) {
		n++;
		x >>= 1;
	}
	return n;
}

static inline ll_unused size_t count_ones(uint64_t x)
{
	size_t c = 0;
	while (x) {
		x &= (x - 1);
		c++;
	}
	return c;
}

