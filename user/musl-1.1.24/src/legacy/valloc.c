#define _BSD_SOURCE
#include <stdlib.h>
#include "libc.h"

void *internel_valloc(size_t size)
{
	return internel_memalign(PAGE_SIZE, size);
}
