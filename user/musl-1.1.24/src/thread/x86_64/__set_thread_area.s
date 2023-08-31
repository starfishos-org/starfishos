/* Copyright 2011-2012 Nicholas J. Kain, licensed under standard MIT license */

.extern chcore_x86_set_thread_area

.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	/* ChCore: current implementation is using wrfsbase */
	call chcore_x86_set_thread_area	
	ret

#if 0
	mov %rdi,%rsi           /* shift for syscall */
	movl $0x1002,%edi       /* SET_FS register */
	movl $158,%eax          /* set fs segment to */
	syscall                 /* arch_prctl(SET_FS, arg)*/
	ret
#endif
