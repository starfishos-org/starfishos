#include <seg.h>
#include <arch/machine/registers.h>
#include <machine.h>
#include "irq_entry.h"

void arch_syscall_init(void)
{
	u64 star;

	star = ((((u64)UCSEG | 0x3) - 16) << 48) | ((u64)KCSEG64 << 32);
	wrmsr(MSR_STAR, star);
	/* setup syscall entry point */
	wrmsr(MSR_LSTAR, (u64)&sys_entry);
	wrmsr(MSR_SFMASK, EFLAGS_TF | EFLAGS_IF);
}
