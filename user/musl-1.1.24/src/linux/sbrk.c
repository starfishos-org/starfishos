#define _BSD_SOURCE
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <chcore/defs.h>
#include "syscall.h"

void *sbrk(intptr_t inc)
{
	if (inc) return (void *)__syscall_ret(-ENOMEM);
	return (void *)__syscall2(SYS_brk, 0, MALLOC_TYPE_DEFAULT);
}
