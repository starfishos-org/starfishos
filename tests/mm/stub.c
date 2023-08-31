#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int physmem_map_num = 2;

void printk(const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	vprintf(fmt, va);
	va_end(va);
}

struct lock;

int lock_init(struct lock *lock) { return 0; }
int lock(struct lock *lock) { return 0; }
int unlock(struct lock *lock) { return 0; }
