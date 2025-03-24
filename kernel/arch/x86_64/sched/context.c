#include <object/thread.h>
#include <sched/sched.h>
#include <machine.h>
#include <arch/machine/registers.h>
#include <arch/machine/smp.h>
#include <mm/kmalloc.h>
#include <seg.h>

void init_thread_ctx(struct thread *thread, u64 stack, u64 func, u32 prio,
                     u32 type, s32 aff)
{
    /* Init thread registers */
    thread->thread_ctx->ec.reg[RSP] = stack;
    thread->thread_ctx->ec.reg[RIP] = func;
    /* Set thread segment (Lower 2 bits are CPL) */
    thread->thread_ctx->ec.reg[DS] = UDSEG | 3;
    thread->thread_ctx->ec.reg[CS] = UCSEG | 3;
    thread->thread_ctx->ec.reg[SS] = UDSEG | 3;

    /* Set thread flag according to type*/
    if (type == TYPE_KERNEL)
        thread->thread_ctx->ec.reg[RFLAGS] = EFLAGS_1;
    else
        thread->thread_ctx->ec.reg[RFLAGS] = EFLAGS_DEFAULT;

    /* Set the priority and state of the thread */
    thread->thread_ctx->prio = prio;
    thread->thread_ctx->state = TS_INIT;

    /* Set the affinity */
    thread->thread_ctx->cpuid = 0;
    thread->thread_ctx->affinity = aff;

    /* Set the thread type */
    thread->thread_ctx->type = type;

    /* Set the budget of the thread */
    if (thread->thread_ctx->sc != NULL) {
        thread->thread_ctx->sc->budget = DEFAULT_BUDGET;
    }

    /* Set kernel stack state */
    thread->thread_ctx->kernel_stack_state = KS_FREE;
    /* Set exiting state */
    thread->thread_ctx->thread_exit_state = TE_RUNNING;
}

u64 arch_get_thread_stack(struct thread *thread)
{
    return thread->thread_ctx->ec.reg[RSP];
}

void arch_set_thread_stack(struct thread *thread, u64 stack)
{
    thread->thread_ctx->ec.reg[RSP] = stack;
}

void arch_set_thread_return(struct thread *thread, u64 ret)
{
    thread->thread_ctx->ec.reg[RAX] = ret;
}

void arch_set_thread_next_ip(struct thread *thread, u64 ip)
{
    thread->thread_ctx->ec.reg[RIP] = ip;
}

u64 arch_get_thread_next_ip(struct thread *thread)
{
    return thread->thread_ctx->ec.reg[RIP];
}

/* The first argument */
void arch_set_thread_arg0(struct thread *thread, u64 arg)
{
    thread->thread_ctx->ec.reg[RDI] = arg;
}

/* The second argument */
void arch_set_thread_arg1(struct thread *thread, u64 arg)
{
    thread->thread_ctx->ec.reg[RSI] = arg;
}

void arch_set_thread_arg2(struct thread *thread, unsigned long arg)
{
    thread->thread_ctx->ec.reg[RDX] = arg;
}

void arch_set_thread_arg3(struct thread *thread, unsigned long arg)
{
    arch_exec_ctx_t *cur_thread_ctx = &thread->thread_ctx->ec;

    /**
     * There are two ways for a thread to enter kernel on x86_64:
     * interrupts/exceptions, and syscall. They also exist on
     * many other platforms of course, but the main problem on x86_64
     * is that they have different calling convention.
     *
     * In interrupts/exceptions calling convention, all registers
     * are preserved/resumed when entering/leaving kernel, and iretq
     * instruction is used to return to user from kernel. The iretq
     * instruction only uses state on stack to resume userspace
     * control flow. So it's completely transparent for userspace code,
     * and it's free for them to use any calling convention.
     *
     * However, in syscall calling convention, sysretq instruction is
     * used to return to userspace. And this instruction would use %rcx
     * as next ip, %r11 as userspace RFLAGS, i.e., kernel code must clobber
     * %rcx, %r11 and %rax(as return value) to call sysretq correctly. So if
     * a thread entered kernel via syscall, it should be aware of syscall
     * calling convention. Besides, when kernel calls functions in those
     * threads, the kernel should also use syscall calling convention
     * because %rcx which serve as the 4th argument in System V calling
     * convention would be clobbered by kernel to invoke sysretq.
     *
     * Comparing how arguments are passed via registers in SystemV and
     * x86_64 syscall calling convention:
     *
     * SystemV: %rdi %rsi %rdx %rcx %r8 %r9
     *
     * x86_64 syscall: %rdi %rsi %rdx **%r10** %r8 %r9
     *
     * So this function is also the only one who needs to be aware of
     * their difference.
     */
    switch (cur_thread_ctx->reg[EC]) {
    case EC_SYSEXIT:
        cur_thread_ctx->reg[R10] = arg;
        break;
    default:
        cur_thread_ctx->reg[RCX] = arg;
        break;
    }
}

void arch_set_thread_tls(struct thread *thread, u64 tls)
{
    thread->thread_ctx->tls_base_reg[TLS_FS] = tls;
}

/* set arch-specific thread state */
void set_thread_arch_spec_state(struct thread *thread)
{
    /* Currently, nothing need to be done in x86-64. */
}

/* set arch-specific thread state for ipc server thread */
void set_thread_arch_spec_state_ipc(struct thread *thread)
{
    /* Currently, nothing need to be done in x86-64. */
}

/*
 * Saving registers related to thread local storage.
 * On x86_64, FS/GS is used by convention.
 */
void switch_tls_info(struct thread *from, struct thread *to)
{
    if (likely((from) && (from->thread_ctx->type > TYPE_KERNEL))) {
        /* Save FS for thread from */
        from->thread_ctx->tls_base_reg[TLS_FS] = __builtin_ia32_rdfsbase64();
        /* Save GS for thread from */
        // TODO: rdmsr triggers VMExit, so we disable it for now
        // from->thread_ctx->tls_base_reg[TLS_GS] = rdmsr(MSR_GSBASE);
    }

    if (likely((to) && (to->thread_ctx->type > TYPE_KERNEL))) {
        /* Restore FS for thread to */
        __builtin_ia32_wrfsbase64(to->thread_ctx->tls_base_reg[TLS_FS]);
        /* Restore GS for thread to */
        // TODO: rdmsr triggers VMExit (if running in QEMU), so we disable it
        // for now wrmsr(MSR_GSBASE, to->thread_ctx->tls_base_reg[TLS_GS]);
    }
}
