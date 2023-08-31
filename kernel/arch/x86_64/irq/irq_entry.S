#include <common/asm.h>
#include <common/debug.h>
#include <seg.h>
#include <arch/machine/registers.h>
#include <arch/machine/smp.h>

.extern syscall_table
.extern finish_switch
.extern do_pending_resched

/* ChCore: only allow to use `syscall/sysret` for invoking system calls */

.macro switch_to_cpu_stack
	movq	%gs:OFFSET_LOCAL_CPU_STACK, %rsp
.endm

.macro switch_to_thread_ctx
	movq	%gs:OFFSET_CURRENT_EXEC_CTX, %rsp
.endm

/*
 * Syscall num:  %rax
 * Arguments:  %rdi, %rsi, %rdx, %r10, %r8, %r9
 * r11 : restore eflags
 * rcx : restore rip
 */
.macro sys_enter
	swapgs
	/* Save syscall num to per_cpu_info */
        movq %rax, %gs:OFFSET_CURRENT_SYSCALL_NO
	/* Use rax to locate the context from per_cpu_info */
        movq %gs:OFFSET_CURRENT_EXEC_CTX, %rax

	/* Begin of saving user registers */
        movq    %r15, 0x10(%rax)
        movq    %r14, 0x18(%rax)
        movq    %r13, 0x20(%rax)
        movq    %r12, 0x28(%rax)
        movq    %rbp, 0x30(%rax)
        movq    %rbx, 0x38(%rax)
	movq    %r10, 0x48(%rax)
	movq    %r9,  0x50(%rax)
	movq    %r8,  0x58(%rax)
	movq    %rdx, 0x70(%rax)
	movq    %rsi, 0x78(%rax)
	movq    %rdi, 0x80(%rax)

	/* syscall instruction saves next user rip in rcx */
        movq    %rcx, 0x98(%rax)    
	/* syscall instruction saves user eflags in r11 */
        movq    %r11, 0xa8(%rax)    
	/* syscall instruction does not modify stack pointer */
        movq    %rsp, 0xb0(%rax)
	/* End of saving user registers */

	/*
	 * Set EC to EC_SYSEXIT:
	 * the user thread enters kernel through syscall;
	 * later, the kernel will return to it through sysret.
	 */
        movq    $EC_SYSEXIT, 0x90(%rax) 

	/* Switch stack to the kernel stack of current thread */
        movq    %rax, %rsp
.endm

/* 
 * sys_exit: uses sysretq to transfer the control flow to a user thread
 */
.macro sys_exit

	/* Begin of restoring user registers */
        movq    0x10(%rsp), %r15
        movq    0x18(%rsp), %r14
        movq    0x20(%rsp), %r13
        movq    0x28(%rsp), %r12
        movq    0x30(%rsp), %rbp
        movq    0x38(%rsp), %rbx
	movq    0x48(%rsp), %r10
	movq    0x50(%rsp), %r9
	movq    0x58(%rsp), %r8
	movq    0x70(%rsp), %rdx
	movq    0x78(%rsp), %rsi
	movq    0x80(%rsp), %rdi

	/* rcx will be used by sysretq to restore rip */
        movq    0x98(%rsp), %rcx    
	/* r11 will be used by sysretq to restore eflags */
        movq    0xa8(%rsp), %r11    
	/* Restore the stack of the user thread */
        movq    0xb0(%rsp), %rsp
	/* End of restoring user registers */

        swapgs
        sysretq
.endm

.code64
.align 8
BEGIN_FUNC(sys_entry)
	sys_enter
#ifndef CHCORE_KERNEL_RT
	switch_to_cpu_stack
#endif

	// TODO: enabling irq here makes the program slower (maybe) due to the frequent timer.
	// FIXME: figure out how to solve this problem.
	// sti

	/* For x86_64 C calling convention, replace r10 with rcx */
        movq    %r10, %rcx      
	/* The syscall num in %gs:OFFSET_CURRENT_SYSCALL_NO */
	movq	%gs:OFFSET_CURRENT_SYSCALL_NO, %rax

	/* Optional tracing: hooking syscall */
#if HOOKING_SYSCALL == ON
	pushq   %rax
	pushq   %rdi
	pushq   %rsi
	pushq   %rdx
	pushq   %r8
	pushq   %r9
	pushq   %r10
	pushq   %rcx
	pushq   %r11

	mov %rax, %rdi
	call hook_syscall

	popq    %r11
	popq    %rcx
	popq    %r10
	popq    %r9
	popq    %r8
	popq    %rdx
	popq    %rsi
	popq    %rdi
	popq    %rax
#endif

	lea	syscall_table, %r15
	movq	(%r15, %rax, 8), %r14
	/* Leave the return value (in %rax) for user space */
        call    *%r14

#ifdef CHCORE_KERNEL_RT
        /*
         * Save return value in memory because a context switch may happen
         * in do_pending_resched.
         */
	movq	%rax, 0x60(%rsp)
	call	do_pending_resched
	movq	0x60(%rsp), %rax
#else
	switch_to_thread_ctx
#endif
	sys_exit
END_FUNC(sys_entry)

BEGIN_FUNC(eret_to_thread_through_sys_exit)
	/*
	 * Set the stack according to the first argument.
	 * Then, the stack is changed to the the kernel stack of
	 * of current thread which will be restored here by sys_exit.
	 */
	movq	%rdi, %rsp

#ifdef CHCORE_KERNEL_RT
	/* Mark the kernel stack of the previous thread as free */
	call finish_switch
#endif

	/*
	 * Not directly returned from a syscall function.
	 * Set the return value according to thread_ctx.
	 */
	movq    0x60(%rsp), %rax

	sys_exit
END_FUNC(eret_to_thread_through_sys_exit)

/*******************
 * TRAP in x86_64
 *******************/

.macro trap_enter
	/*
	 * When trapping from a user thread,
	 * now %rsp is just above TRAPNO.
	 * See kernel/include/arch/x86_64/arch/machine/registers.h
	 * 
	 * Besides, when a trap happens in kernel,
	 * the stack will not change and this procedure is also used
	 * for saving the kernel trap/interrupt context.
	 */

	/* Save the context at the trap/interrupt time */
        pushq	%rdi
        pushq	%rsi
        pushq	%rdx
        pushq	%rcx
        pushq	%rax
        pushq	%r8
        pushq	%r9
        pushq	%r10
        pushq	%r11
        pushq	%rbx
        pushq	%rbp
        pushq	%r12
        pushq	%r13
        pushq	%r14
        pushq	%r15

	/* Skip the non-used area and thus set rsp to the kernel stack */
        subq	$0x10, %rsp
.endm

.macro trap_exit

	/*
	 * When restoring a user thread,
	 * skip the non-used area and thus set rsp to the exec_cont.
	 * 
	 * Besides, when a trap happens in kernel,
	 * the stack will not change and this procedure is also used
	 * for restoring the kernel execution.
	 */
	addq	$0x10, %rsp

	/* Restore the execution context */
        popq	%r15
        popq	%r14
        popq	%r13
        popq	%r12
        popq	%rbp
        popq	%rbx
        popq	%r11
        popq	%r10
        popq	%r9
        popq	%r8
        popq	%rax
        popq	%rcx
        popq	%rdx
        popq	%rsi
        popq	%rdi

	/* Skip the trapno and ec */
	addq	$0x10, %rsp	

        iretq
.endm

.macro trap_handler
	trap_enter

	/*
	 * Now %rsp points to the execution context constructed
	 * by the above trap_enter.
	 */
	movq	%rsp, %rdi	

	/* 
	 * When a trap happens, CPU stores CS in 0xa0(%rsp):
	 * See kernel/include/arch/x86_64/arch/machine/registers.h
	 */
        cmpw	$KCSEG64, 0xa0(%rsp)	

	/*
	 * When the above cmp gets zero,
	 * it means the trap happens during the kernel execution.
	 * Thus, skip the swapgs.
	 */
        jz	1f
        swapgs
#ifndef CHCORE_KERNEL_RT
	switch_to_cpu_stack
#endif
1:      	
        call	trap_c

#ifdef CHCORE_KERNEL_RT
        cmpw	$KCSEG64, 0xa0(%rsp)
	jz	2f
	call	do_pending_resched
#else
	cmpq	%gs:OFFSET_LOCAL_CPU_STACK, %rsp
	jnz	2f
	switch_to_thread_ctx
#endif
        swapgs
2:      
	trap_exit
.endm


.macro	trap_entry_no_ec no
	.globl trap_\no
        .align 8
trap_\no:

	/*
	 * Since this kind of trap has no EC (error code),
	 * we set EC to 0.
	 * Note the we set EC to -1 (EC_SYSEXIT) in `sys_enter`.
	 */
	pushq	$0		
	/* Push the trapno (i.e., TRAPNO in the exec_cont) */
	pushq	$\no
	/* Invoke the trap_hanler */
	trap_handler

	.data
        .quad	trap_\no
	.previous
.endm

.macro	trap_entry_ec no
	.globl trap_\no
        .align 8
trap_\no:
	/* Push the trapno (i.e., TRAPNO in the exec_cont) */
	pushq	$\no
	/* Invoke the trap_hanler */
	trap_handler

        .data
        .quad	trap_\no
        .previous
.endm

BEGIN_FUNC(eret_to_thread_through_trap_exit)

	/* restore rsp from first argument */
	movq	%rdi, %rsp
#ifdef CHCORE_KERNEL_RT
	call finish_switch
#endif

	cmpw	$KCSEG64, 0xa0(%rsp)	/* cs in context */
	jz	1f
	swapgs
1:	trap_exit
END_FUNC(eret_to_thread_through_trap_exit)


/* Begin of idt_entry */
.data
.align 8
EXPORT(idt_entry)
.text

trap_entry_no_ec 0
trap_entry_no_ec 1
trap_entry_no_ec 2
trap_entry_no_ec 3
trap_entry_no_ec 4
trap_entry_no_ec 5
trap_entry_no_ec 6
trap_entry_no_ec 7
trap_entry_ec 	 8
trap_entry_no_ec 9
trap_entry_ec 	 10
trap_entry_ec 	 11
trap_entry_ec 	 12
trap_entry_ec 	 13
trap_entry_ec 	 14
trap_entry_no_ec 15
trap_entry_no_ec 16
trap_entry_ec 	 17
trap_entry_no_ec 18
trap_entry_no_ec 19
trap_entry_no_ec 20
trap_entry_no_ec 21
trap_entry_no_ec 22
trap_entry_no_ec 23
trap_entry_no_ec 24
trap_entry_no_ec 25
trap_entry_no_ec 26
trap_entry_no_ec 27
trap_entry_no_ec 28
trap_entry_no_ec 29
trap_entry_no_ec 30
trap_entry_no_ec 31
trap_entry_no_ec 32
trap_entry_no_ec 33
trap_entry_no_ec 34
trap_entry_no_ec 35
trap_entry_no_ec 36
trap_entry_no_ec 37
trap_entry_no_ec 38
trap_entry_no_ec 39
trap_entry_no_ec 40
trap_entry_no_ec 41
trap_entry_no_ec 42
trap_entry_no_ec 43
trap_entry_no_ec 44
trap_entry_no_ec 45
trap_entry_no_ec 46
trap_entry_no_ec 47
trap_entry_no_ec 48
trap_entry_no_ec 49
trap_entry_no_ec 50
trap_entry_no_ec 51
trap_entry_no_ec 52
trap_entry_no_ec 53
trap_entry_no_ec 54
trap_entry_no_ec 55
trap_entry_no_ec 56
trap_entry_no_ec 57
trap_entry_no_ec 58
trap_entry_no_ec 59
trap_entry_no_ec 60
trap_entry_no_ec 61
trap_entry_no_ec 62
trap_entry_no_ec 63
trap_entry_no_ec 64
trap_entry_no_ec 65
trap_entry_no_ec 66
trap_entry_no_ec 67
trap_entry_no_ec 68
trap_entry_no_ec 69
trap_entry_no_ec 70
trap_entry_no_ec 71
trap_entry_no_ec 72
trap_entry_no_ec 73
trap_entry_no_ec 74
trap_entry_no_ec 75
trap_entry_no_ec 76
trap_entry_no_ec 77
trap_entry_no_ec 78
trap_entry_no_ec 79
trap_entry_no_ec 80
trap_entry_no_ec 81
trap_entry_no_ec 82
trap_entry_no_ec 83
trap_entry_no_ec 84
trap_entry_no_ec 85
trap_entry_no_ec 86
trap_entry_no_ec 87
trap_entry_no_ec 88
trap_entry_no_ec 89
trap_entry_no_ec 90
trap_entry_no_ec 91
trap_entry_no_ec 92
trap_entry_no_ec 93
trap_entry_no_ec 94
trap_entry_no_ec 95
trap_entry_no_ec 96
trap_entry_no_ec 97
trap_entry_no_ec 98
trap_entry_no_ec 99
trap_entry_no_ec 100
trap_entry_no_ec 101
trap_entry_no_ec 102
trap_entry_no_ec 103
trap_entry_no_ec 104
trap_entry_no_ec 105
trap_entry_no_ec 106
trap_entry_no_ec 107
trap_entry_no_ec 108
trap_entry_no_ec 109
trap_entry_no_ec 110
trap_entry_no_ec 111
trap_entry_no_ec 112
trap_entry_no_ec 113
trap_entry_no_ec 114
trap_entry_no_ec 115
trap_entry_no_ec 116
trap_entry_no_ec 117
trap_entry_no_ec 118
trap_entry_no_ec 119
trap_entry_no_ec 120
trap_entry_no_ec 121
trap_entry_no_ec 122
trap_entry_no_ec 123
trap_entry_no_ec 124
trap_entry_no_ec 125
trap_entry_no_ec 126
trap_entry_no_ec 127
trap_entry_no_ec 128
trap_entry_no_ec 129
trap_entry_no_ec 130
trap_entry_no_ec 131
trap_entry_no_ec 132
trap_entry_no_ec 133
trap_entry_no_ec 134
trap_entry_no_ec 135
trap_entry_no_ec 136
trap_entry_no_ec 137
trap_entry_no_ec 138
trap_entry_no_ec 139
trap_entry_no_ec 140
trap_entry_no_ec 141
trap_entry_no_ec 142
trap_entry_no_ec 143
trap_entry_no_ec 144
trap_entry_no_ec 145
trap_entry_no_ec 146
trap_entry_no_ec 147
trap_entry_no_ec 148
trap_entry_no_ec 149
trap_entry_no_ec 150
trap_entry_no_ec 151
trap_entry_no_ec 152
trap_entry_no_ec 153
trap_entry_no_ec 154
trap_entry_no_ec 155
trap_entry_no_ec 156
trap_entry_no_ec 157
trap_entry_no_ec 158
trap_entry_no_ec 159
trap_entry_no_ec 160
trap_entry_no_ec 161
trap_entry_no_ec 162
trap_entry_no_ec 163
trap_entry_no_ec 164
trap_entry_no_ec 165
trap_entry_no_ec 166
trap_entry_no_ec 167
trap_entry_no_ec 168
trap_entry_no_ec 169
trap_entry_no_ec 170
trap_entry_no_ec 171
trap_entry_no_ec 172
trap_entry_no_ec 173
trap_entry_no_ec 174
trap_entry_no_ec 175
trap_entry_no_ec 176
trap_entry_no_ec 177
trap_entry_no_ec 178
trap_entry_no_ec 179
trap_entry_no_ec 180
trap_entry_no_ec 181
trap_entry_no_ec 182
trap_entry_no_ec 183
trap_entry_no_ec 184
trap_entry_no_ec 185
trap_entry_no_ec 186
trap_entry_no_ec 187
trap_entry_no_ec 188
trap_entry_no_ec 189
trap_entry_no_ec 190
trap_entry_no_ec 191
trap_entry_no_ec 192
trap_entry_no_ec 193
trap_entry_no_ec 194
trap_entry_no_ec 195
trap_entry_no_ec 196
trap_entry_no_ec 197
trap_entry_no_ec 198
trap_entry_no_ec 199
trap_entry_no_ec 200
trap_entry_no_ec 201
trap_entry_no_ec 202
trap_entry_no_ec 203
trap_entry_no_ec 204
trap_entry_no_ec 205
trap_entry_no_ec 206
trap_entry_no_ec 207
trap_entry_no_ec 208
trap_entry_no_ec 209
trap_entry_no_ec 210
trap_entry_no_ec 211
trap_entry_no_ec 212
trap_entry_no_ec 213
trap_entry_no_ec 214
trap_entry_no_ec 215
trap_entry_no_ec 216
trap_entry_no_ec 217
trap_entry_no_ec 218
trap_entry_no_ec 219
trap_entry_no_ec 220
trap_entry_no_ec 221
trap_entry_no_ec 222
trap_entry_no_ec 223
trap_entry_no_ec 224
trap_entry_no_ec 225
trap_entry_no_ec 226
trap_entry_no_ec 227
trap_entry_no_ec 228
trap_entry_no_ec 229
trap_entry_no_ec 230
trap_entry_no_ec 231
trap_entry_no_ec 232
trap_entry_no_ec 233
trap_entry_no_ec 234
trap_entry_no_ec 235
trap_entry_no_ec 236
trap_entry_no_ec 237
trap_entry_no_ec 238
trap_entry_no_ec 239
trap_entry_no_ec 240
trap_entry_no_ec 241
trap_entry_no_ec 242
trap_entry_no_ec 243
trap_entry_no_ec 244
trap_entry_no_ec 245
trap_entry_no_ec 246
trap_entry_no_ec 247
trap_entry_no_ec 248
trap_entry_no_ec 249
trap_entry_no_ec 250
trap_entry_no_ec 251
trap_entry_no_ec 252
trap_entry_no_ec 253
trap_entry_no_ec 254
trap_entry_no_ec 255

/* End of idt_entry */
