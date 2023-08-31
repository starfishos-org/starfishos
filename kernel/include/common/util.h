#pragma once

#include <common/types.h>

#ifdef CHCORE
/*
 * memcpy does not handle: dst and src overlap.
 * memmove does.
 */
void memmove(void *dst, const void *src, size_t size);

/* use bzero instead of memset(buf, 0, size) */
void bzero(void *p, size_t size);

static inline void memset(void *dst, char ch, size_t size)
{
	size_t i;
	char *dst_ch = dst;

	for (i = 0; i < size; ++i) {
		dst_ch[i] = ch;
	}
}

static inline void memcpy(void *dst, const void *src, size_t size)
{
	s64 i;
	char *dst_ch = dst;
	const char *src_ch = src;

	for (i = size - 1; i >= 0; --i) {
		dst_ch[i] = src_ch[i];
	}
}

static inline int strcmp(const char *src, const char *dst)
{
	while (*src && *dst) {
		if (*src == *dst) {
			src++;
			dst++;
			continue;
		}
		return *src - *dst;
	}
	if (!*src && !*dst)
		return 0;
	if (!*src)
		return -1;
	return 1;
}

static inline int strncmp(const char *src, const char *dst, size_t size)
{
	size_t i;

	for (i = 0; i < size; ++i) {
		if (src[i] == '\0' || src[i] != dst[i])
			return src[i] - dst[i];
	}

	return 0;
}

static inline size_t strlen(const char *src)
{
	size_t i = 0;

	while (*src++)
		i++;

	return i;
}
#endif
