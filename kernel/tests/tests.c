#include <common/lock.h>
#include <common/kprint.h>
#include <arch/machine/smp.h>
#include <common/macro.h>
#include <mm/kmalloc.h>

#include "tests.h"

void run_test(void)
{
	tst_mutex();
	tst_rwlock();
	tst_malloc();
	tst_sched();
}
