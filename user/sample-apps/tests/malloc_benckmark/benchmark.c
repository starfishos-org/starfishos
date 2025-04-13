
#include "benchmark.h"
#include <stdlib.h>
#include <rpmalloc.h>
#include <string.h>

int
benchmark_initialize() {
	return 0;
}

int
benchmark_finalize(void) {
	return 0;
}

int
benchmark_thread_initialize(void) {
	return 0;
}

int
benchmark_thread_finalize(void) {
	return 0;
}

void
benchmark_thread_collect(void) {
	rpmalloc_thread_collect();
}

#pragma GCC push_options
#pragma GCC optimize("O0")
void*
benchmark_malloc(size_t alignment, size_t size) {
	// return rpmemalign(alignment, size);
	void* ptr = malloc(size);
	// access the first byte of the allocated memory
	char dst[size];
	memcpy(dst, ptr, size);
	(void)dst;
	return ptr;
}
#pragma GCC pop_options

extern void
benchmark_free(void* ptr) {
	free(ptr);
}

const char*
benchmark_name(void) {
#if defined(ENABLE_UNLIMITED_CACHE)
	return "rpmalloc-unlimit";
#elif defined(DISABLE_CACHE)
	return "rpmalloc-nocache";
#elif defined(ENABLE_SPACE_PRIORITY_CACHE)
	return "rpmalloc-size";
#else
	return "rpmalloc";
#endif
}
