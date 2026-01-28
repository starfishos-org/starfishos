#include "irq_entry.h"
#include <arch/x2apic.h>
#include <seg.h>
#include <machine.h>
#include <common/kprint.h>
#include <common/util.h>
#include <common/macro.h>
#include <arch/sched/arch_sched.h>
#include <arch/io.h>
#include <arch/time.h>
#include <irq/timer.h>
#include <object/thread.h>
#include <object/irq.h>
#include <irq/irq.h>
#include <irq/ipi.h>
#include <sched/sched.h>
#include <sched/fpu.h>
#include <mm/vmspace.h>
#include <common/vars.h>
#include <sched/fpu.h>
#include <drivers/ivshmem.h>

/* idt and idtr */
struct gate_desc idt[T_NUM] __attribute__((aligned(16)));
struct pseudo_desc idtr = {sizeof(idt) - 1, (u64)idt};

/* record irq is handled by kernel or user */
u8 irq_handle_type[MAX_IRQ_NUM];

/* kernel stack used when shutting down */
char shutdown_kernel_stack[PLAT_CPU_NUM][CPU_STACK_SIZE];

extern void flush_tlb_all(void);

void arch_enable_irq(void)
{
    asm volatile("sti");
}

void arch_disable_irq(void)
{
    asm volatile("cli");
}

#define PIC1_BASE 0x20
#define PIC2_BASE 0xa0

void initpic(void)
{
    put8(PIC1_BASE + 1, 0xff);
    put8(PIC2_BASE + 1, 0xff);
}

void arch_enable_irqno(int irq)
{
    BUG("Not impl.");
}

void arch_disable_irqno(int irq)
{
    BUG("Not impl.");
}

void arch_interrupt_init_per_cpu(void)
{
    u32 eax, ebx, ecx, edx;

    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (ecx & FEATURE_ECX_X2APIC)
        x2apic_init();
    else if (edx & FEATURE_EDX_XAPIC)
        BUG("xapic init not implemented\n");
    else
        BUG("not apic detected\n");
    arch_disable_irq();
    initpic();
    asm volatile("lidt (%0)" : : "r"(&idtr));
}

void clear_idt_entry()
{
    memset(idt, 0, sizeof(idt));
    asm volatile("lidt (%0)" : : "r"(&idtr));
}

void arch_interrupt_init(void)
{
    int i = 0;

    /* Set up interrupt gates */
    for (i = 0; i < T_NUM; i++) {
        /* Set all gate to interrupt gate to be effected by
         * eflags.interrupt_enable */
        if (i == T_BP) {
            set_gate(idt[i], GT_INTR_GATE, KCSEG64, idt_entry[i], 3);
        } else {
            set_gate(idt[i], GT_INTR_GATE, KCSEG64, idt_entry[i], 0);
        }
    }
    arch_interrupt_init_per_cpu();

    memset(irq_handle_type, HANDLE_KERNEL, MAX_IRQ_NUM);
}

/* Mark the end of an IRQ */
void arch_ack_irq()
{
    x2apic_eoi();
}

/*
 * This interface is only for the common interface across different
 * architectures.
 */
void plat_ack_irq(int irq)
{
    arch_ack_irq();
}

void plat_set_next_timer(u64 tick_delta)
{
    u64 tsc;
    tsc = get_cycles();
    wrmsr(MSR_IA32_TSC_DEADLINE, tsc + tick_delta);
}

void plat_handle_timer_irq(u64 tick_delta)
{
    plat_set_next_timer(tick_delta);
    arch_ack_irq();
}

void plat_disable_timer(void)
{
    x2apic_disable_timer();
}

void plat_enable_timer(void)
{
    x2apic_enable_timer();
}

void stop_and_resched(u32 cpuid)
{
    mark_in_kernel_ipi_tx(cpuid);
    arch_ack_irq();
    sched();
    eret_to_thread(switch_context());
}

void handle_reset_sched(u32 cpuid)
{
    current_thread = NULL;

    u64 tmp_kernel_stack = (u64)shutdown_kernel_stack[cpuid + 1];
    asm volatile("mov %0, %%rsp" : : "r"(tmp_kernel_stack) :);

    /* handle irq using the new stack */
    mark_in_kernel_ipi_tx(cpuid);
    arch_ack_irq();
    u64 value = 0;
    asm volatile("movq %0, %%gs:%c1"
                 :
                 : "r"(value), "i"(OFFSET_CURRENT_FPU_OWNER)
                 : "memory");

    wait_finish_in_kernel(cpuid);

    plat_timer_init();
    flush_tlb_all();

    sched();
    eret_to_thread(switch_context());

    /* should never return */
    BUG_ON(1);
}

void handle_wait_in_kernel(u32 cpuid)
{
    if (current_thread->thread_ctx->is_fpu_owner >= 0) {
        save_and_release_fpu(current_thread, smp_get_cpu_id());
    }
    /* Save FS for thread from */
    if (likely((current_thread)
               && (current_thread->thread_ctx->type > TYPE_KERNEL))) {
        current_thread->thread_ctx->tls_base_reg[TLS_FS] =
                __builtin_ia32_rdfsbase64();
    }
    mark_in_kernel_ipi_tx(cpuid);
    wait_finish_in_kernel(cpuid);
    flush_tlb_all();
}

void handle_irq(int irqno)
{
    int r;

    BUG_ON(irqno >= MAX_IRQ_NUM);
    if (irq_handle_type[irqno] == HANDLE_USER) {
        r = user_handle_irq(irqno);
        BUG_ON(r);
        return;
    }

    switch (irqno) {
    case IRQ_TIMER:
        handle_timer_irq();

        /* Start the scheduler */
        sched();
        eret_to_thread(switch_context());
        return;

    case IRQ_IPI_TLB:
    case IRQ_IPI_TLB_BATCH:
    case IRQ_IPI_RESCHED:
    case IRQ_IPI_WAIT_IN_KERNEL:
        // kinfo("CPU %d: receive IPI on TLB.\n", smp_get_cpu_id());
        handle_ipi();
        arch_ack_irq();
        if (current_resched_flag == true) {
            current_resched_flag = false;
            sched();
            eret_to_thread(switch_context());

            /* should never return */
            BUG_ON(1);
        }
        return;

    case IRQ_MSIX_IVSHMEM:
        /* Handle MSI-X interrupt from ivshmem-doorbell */
        /* Note: This interrupt may arrive before full initialization */
        /* The handler itself will check for NULL pointers */
        ivshmem_msix_handler();
        arch_ack_irq();
        return;

    case IRQ_IPI_RESET_SCHED:
        handle_ipi();
        return;
    case IRQ_IPI_STOP_RESCHED:
        /* Start the scheduler */
        handle_ipi();
        arch_ack_irq();
        sched();
        eret_to_thread(switch_context());
        return;
    /* should never return */
    default:
        kwarn("Unkown Exception\n");
    }
}

static inline vaddr_t get_fault_addr()
{
    vaddr_t addr;

    asm volatile("mov %%cr2, %0" : "=r"(addr)::);
    return addr;
}

#ifdef TRACK_TIME
void thread_measure(struct thread *thread, int tracking_state)
{
    if (!thread) {
        thread = current_thread;
        if (!thread || thread->thread_ctx->type == TYPE_IDLE) {
            return;
        }
    }
    u64 ns = plat_get_mono_time();
    u64 duration;
    switch (thread->tracking) {
    case 1: // kernel
        duration = ns - thread->timepoint_ns;
        thread->track_time_kernel += duration;
        break;
    case 2: // user
        duration = ns - thread->timepoint_ns;
        thread->track_time_user += duration;
        break;
    }
    thread->timepoint_ns = ns;
    thread->tracking = tracking_state;
    // if (!strcmp(thread->cap_group->cap_group_name, "/ustress.bin"))
    // 	printk("[dbg] sw [%s] thread to %d\n",
    // thread->cap_group->cap_group_name, tracking_state);
}

long thread_measure_pass(long ret, long state)
{
    thread_measure(NULL, state);
    return ret;
}
#endif

void trap_c(arch_exec_ctx_t *ec)
{
    int trapno = ec->reg[TRAPNO];
    int errorcode = ec->reg[EC];

    if (current_thread) {
        current_thread->thread_ctx->tls_base_reg[TLS_FS] =
                __builtin_ia32_rdfsbase64();
#ifdef TRACK_TIME
        if (current_thread->thread_ctx->type != TYPE_IDLE) {
            thread_measure(current_thread, 1);
        }
#endif
    }
    /*
     * When received IRQ in kernel
     * When current_thread == TYPE_IDLE
     * 	We should handle everything like user thread.
     * Otherwice
     * 	We should ignore the timer, handle other IRQ as normal.
     */
    if (ec->reg[CS] == KCSEG64 && /* Trigger IRQ in kernel */
        current_thread) { /* Has running thread */
        BUG_ON(!current_thread->thread_ctx);
        if (current_thread->thread_ctx->type != TYPE_IDLE) {
            /* And the thread is not the IDLE thread */
            if (trapno == IRQ_TIMER) {
                /* We do not allow kernel preemption */
                /* TODO: dynamic high resulution timer */
                plat_handle_timer_irq(TICK_MS * 1000 * tick_per_us);
                return;
            }
            /* For MSI-X interrupts, always handle them even in kernel mode */
            /* This allows interrupts to break into kernel execution */
            if (trapno == IRQ_MSIX_IVSHMEM) {
                /* MSI-X interrupts should be handled immediately */
                handle_irq(trapno);
                return;
            }
        }
    }

    if (trapno == T_GP) {
        static int cnt = 0;
        if (cnt == 0) {
            cnt += 1;
            kinfo("General Protection Fault\n");
            kinfo("Faulting Address: 0x%lx\n", get_fault_addr());
            kinfo("Current thread %p\n", current_thread);
            kinfo("Trap from IP 0x%lx EC %d Trap No. %d\n",
                  ec->reg[RIP],
                  errorcode,
                  trapno);
            kinfo("DS 0x%x, CS 0x%x, RSP 0x%lx, SS 0x%x\n",
                  ec->reg[DS],
                  ec->reg[CS],
                  ec->reg[RSP],
                  ec->reg[SS]);
            kinfo("rax: 0x%lx, rdx: 0x%lx, rdi: 0x%lx\n",
                  ec->reg[RAX],
                  ec->reg[RDX],
                  ec->reg[RDI]);
            kinfo("rcx: 0x%lx\n", ec->reg[RCX]);

            kprint_vmr(current_thread->vmspace);

            kinfo("process: %p\n", current_cap_group);
            print_thread(current_thread);
            while (1)
                ;
        }
        // kinfo("General Protection Fault\n");
        while (1)
            ;
    }

    /* Just for kernel tracing and debugging */
    if ((trapno != IRQ_TIMER) && (trapno != T_PF) && (trapno != IRQ_IPI_TLB)
        && (trapno != IRQ_IPI_TLB_BATCH) && (trapno != IRQ_IPI_RESCHED)
        && (trapno != IRQ_IPI_WAIT_IN_KERNEL)
        && (trapno != IRQ_IPI_STOP_RESCHED) && (trapno != IRQ_IPI_RESET_SCHED)
        && (trapno != IRQ_MSIX_IVSHMEM) && (trapno != T_NM)) {
        kinfo("Trap from IP 0x%lx EC %d Trap No. %d\n",
              ec->reg[RIP],
              errorcode,
              trapno);
        kinfo("DS 0x%x, CS 0x%x, RSP 0x%lx, SS 0x%x\n",
              ec->reg[DS],
              ec->reg[CS],
              ec->reg[RSP],
              ec->reg[SS]);
        kinfo("rax: 0x%lx, rdx: 0x%lx, rdi: 0x%lx\n",
              ec->reg[RAX],
              ec->reg[RDX],
              ec->reg[RDI]);
        BUG_ON(1);
    }

#ifdef IPC_PERF_TRAP
    extern volatile bool ipc_perf_enabled;
    u64 begin = 0, end = 0;
    (void)begin;
    (void)end;
    if (ipc_perf_enabled) {
        begin = plat_get_mono_time();
    }
#endif

    switch (trapno) {
    case T_DE:
        kinfo("Divide Error\n");
        while (1)
            ;
        break;
    case T_DB:
        kinfo("Debug Exception\n");
        return;
    case T_NMI:
        kinfo("Non-maskable Interrupt\n");
        break;
    case T_BP:
        kinfo("Break Point\n");
        break;
    case T_OF:
        kinfo("Overflow\n");
        break;
    case T_BR:
        kinfo("Bounds Range Check\n");
        break;
    case T_UD:
        kinfo("Undefined Opcode\n");
        break;
    case T_NM:
        kdebug("Device (ChCore considers FPU only) Not Available:\n");
#if FPU_SAVING_MODE == LAZY_FPU_MODE
        change_fpu_owner(current_thread);
#ifdef IPC_PERF_TRAP
        goto perf_end;
#endif
        return;
#else
        break;
#endif
    case T_DF:
        kinfo("Double Fault\n");
        break;
    case T_CSO:
        kinfo("Coprocessor Segment Overrun\n");
        break;
    case T_TS:
        kinfo("Invalid Task Switch Segment\n");
        break;
    case T_NP:
        kinfo("Segment Not Present\n");
        break;
    case T_SS:
        kinfo("Stack Exception\n");
        break;
    case T_GP: {
        kinfo("General Protection Fault\n");
        while (1)
            ;
        break;
    }
    case T_PF: {
        kdebug("Page Fault\n");
        /* Page Fault Handler Here! */
        do_page_fault(errorcode, ec->reg[RIP]);
        return;
        // break;
    }
    case T_MF:
        kinfo("Floating Point Error\n");
        break;
    case T_AC:
        kinfo("Alignment Check\n");
        break;
    case T_MC:
        kinfo("Machine Check\n");
        break;
    case T_XM:
        kinfo("SIMD Floating Point Error\n");
        break;
    case T_VE:
        kinfo("Virtualization Exception\n");
        break;
    default:
        handle_irq(trapno);
#ifdef IPC_PERF_TRAP
        goto perf_end;
#endif
        return;
    }

    /*
     * After handling the interrupts,
     * we directly resume the execution.
     *
     * Rescheduling only happens after IRQ_TIMER or IRQ_IPI_RESCHED.
     */

#ifdef IPC_PERF_TRAP
perf_end:
    if (ipc_perf_enabled) {
        end = plat_get_mono_time();
        extern volatile u64 ipc_perf_sum_count;
        extern volatile u64 ipc_perf_sum_time;
        ipc_perf_sum_count += 1;
        ipc_perf_sum_time += end - begin;
    }
#endif

    return;
}

void __eret_to_thread(u64 sp)
{
#ifdef TRACK_TIME
    if (current_thread->thread_ctx->type != TYPE_IDLE) {
        thread_measure(current_thread, 2); // user
    }
#endif
    struct thread_ctx *cur_thread = (struct thread_ctx *)sp;
    arch_exec_ctx_t *cur_thread_ctx = &cur_thread->ec;

    switch (cur_thread_ctx->reg[EC]) {
    case EC_SYSEXIT:
        eret_to_thread_through_sys_exit(sp);
        break;
    default:
        eret_to_thread_through_trap_exit(sp);
        break;
    }
    /* Non-reachable here */
}
