#include <chcore/defs.h>

.global __unmapself
.type   __unmapself,%function
__unmapself:
	/*
	 * We implement mmap/munmap in user space. But when a thread exit, it
	 * will call unmapself to recycle its tls and stack. So we need to switch
	 * the stack of the thread to a common stack, and then we release it's
	 * stack. When all the work is done, we release the common stack.
	 */
	mov	x19, x0		/* Save unmap addr. */
	mov	x20, x1		/* Save unmap length. */
	bl	chcore_lock_common_stack
	mov	sp, x0		/* Switch the stack. */

	mov	x0, x19
	mov	x1, x20
	bl	chcore_unmapself

	/* Release lock of common stack. */
	dmb	ish
	str	wzr, [x0]
	dmb	ish

	mov	x8, #CHCORE_SYS_thread_exit
	svc	0
