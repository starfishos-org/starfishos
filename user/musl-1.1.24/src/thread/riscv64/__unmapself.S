#include <chcore/defs.h>

.global __unmapself
.type __unmapself, %function
__unmapself:
	/*
	 * We implement mmap/munmap in user space. But when a thread exit, it
	 * will call unmapself to recycle its tls and stack. So we need to switch
	 * the stack of the thread to a common stack, and then we release it's
	 * stack. When all the work is done, we release the common stack.
	 */
	mv	s2, a0		/* Save unmap addr. */
	mv	s3, a1		/* Save unmap length. */
	jal	chcore_lock_common_stack
	mv	sp, a0		/* Switch the stack. */

	mv	a0, s2
	mv	a1, s3
	jal	chcore_unmapself

	/* Release lock of common stack. */
	fence	rw, rw
	sw	zero, 0(a0)
	fence	rw, rw

	li	a7, CHCORE_SYS_thread_exit
	ecall
