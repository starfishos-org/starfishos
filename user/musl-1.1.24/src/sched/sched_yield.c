#include <sched.h>
#include "syscall.h"

int sched_yield()
{
	return __syscall(SYS_sched_yield);
}
