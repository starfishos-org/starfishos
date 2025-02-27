#pragma once

/* Leave 8K space to per-CPU stack */
#define CPU_STACK_SIZE  (8192 * 10)
#define STACK_ALIGNMENT 16

// #include <arch/mmu.h>
/* can be different in different architectures */
#ifndef KBASE
#define KBASE 0xffffff0000000000
#endif
