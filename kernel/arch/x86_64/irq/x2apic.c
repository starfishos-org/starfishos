#include "irq_entry.h"

#include <common/kprint.h>
#include <machine.h>
#include <sched/sched.h>
#include <arch/time.h>
#include <arch/machine/smp.h>

/* X2APIC is the most widely-used variant of XAPIC (aka., APIC) on x86/64
 * platforms */

#define IA32_X2APIC_VER        0x803
#define IA32_X2APIC_EOI        0x80b
#define IA32_X2APIC_SPIV       0x80f
#define APIC_VECTOR_MASK       0xff
#define APIC_SPIV_APIC_ENABLED (1 << 8)
#define IA32_X2APIC_ESR        0x828

/*
 * Interrupt Command Register (ICR).
 * In x2APIC mode, the ICR uses MSR 830H.
 */
#define X2APIC_ICR 0x830

/*
 * The ICR consists of the following fields.
 * vector: bits 0-7 (the vector number of the interrupt being sent).
 * Delivery Mode: bits 8-10 (specify the "type" of IPI to be sent).
 */
#define ICR_TYPE_FIXED    0x000
#define ICR_TYPE_LOW_PRIO 0x100
#define ICR_TYPE_SMI      0x200
#define ICR_TYPE_RESERVE  0x300
#define ICR_TYPE_NMI      0x400
#define ICR_TYPE_INIT     0x500 /* processor INIT */
#define ICR_TYPE_STARTUP  0x600 /* "start-up" IPI */

/* Destination Mode: bit 11 */
#define ICR_DEST_PHYSICAL (0x0 << 11)
#define ICR_DEST_LOGICAL  (0x1 << 11)

/*
 * Delivery Status: bit 12 (read-only).
 * 0: idle; 1: send pending
 */

/*
 * Level: bit 14.
 * 0: De-assert; 1: assert
 */
#define ICR_LEVEL_ASSERT (0x1 << 14)

/*
 * Tigger Mode: bit 15.
 * 0: Edge; 1: Level
 */
#define ICR_TRIGGER_LEVEL (0x1 << 15)

/*
 * Destination Shorthand: bits 18-19.
 */
#define ICR_DEST_NO       (0x0 << 18)
#define ICR_DEST_SELF     (0x1 << 18)
#define ICR_DEST_ALL_SELF (0x2 << 18)
#define ICR_DEST_ALL      (0x3 << 18)

/*
 * Destination: bits 32-63.
 * When Shorthand is ICR_DEST_NO.
 */

#define IA32_X2APIC_LVT_TIMER  0x832
#define LVT_TIMER_ONESHOT      (0 << 17)
#define LVT_TIMER_PERIODIC     (1 << 17)
#define LVT_TIMER_TSCDEADLINE  (2 << 17)
#define IA32_X2APIC_INIT_COUNT 0x838
#define IA32_X2APIC_CUR_COUNT  0x839
#define IA32_X2APIC_DIV_CONF   0x83e
#define APIC_LVT_MASKED        (1 << 16)

u64 cur_freq = 0;
u64 tick_per_us = 0;
u64 cntv_tval = 0;

u64 cntv_init;

void x2apic_init_timer()
{
        u64 tsc;

        if (cur_freq == 0) {
                cur_freq = get_tsc_through_pit();
                kinfo("[ChCore] cpu frequency=(Dec)%llu\n", cur_freq);
                tick_per_us = cur_freq / 1000 / 1000;

                cntv_init = get_cycles();
        }

        /* frequency divided by 1 */
        wrmsr(IA32_X2APIC_DIV_CONF, 0xb);
        /* set local timer irq */
        wrmsr(IA32_X2APIC_LVT_TIMER, LVT_TIMER_TSCDEADLINE | IRQ_TIMER);
        tsc = get_cycles();
        wrmsr(MSR_IA32_TSC_DEADLINE, tsc + cur_freq * TICK_MS / 1000);

        wrmsr(IA32_X2APIC_EOI, 0);
}

/*Return the mono time using nano-seconds */
#define NS_IN_MS (1000000UL)
u64 plat_get_mono_time(void)
{
        u64 tsc;
        tsc = get_cycles();

        return (tsc - cntv_init) * NS_IN_US / tick_per_us;
}

u64 plat_get_current_tick(void)
{
        return get_cycles();
}

void x2apic_eoi(void)
{
        wrmsr(IA32_X2APIC_EOI, 0);
}

void x2apic_init()
{
        u64 apic_base = rdmsr(IA32_APIC_BASE);

        /* BIOS enable x2apic with x2apic id greater than 255 */
        if (apic_base & APIC_BASE_X2APEC_ENABLE)
                kwarn("x2apic already enabled\n");
        else
                wrmsr(IA32_APIC_BASE, apic_base | APIC_BASE_X2APEC_ENABLE);

        /* Enable lapic in SVR. Set spuriouse vector to max of irq. */
        wrmsr(IA32_X2APIC_SPIV, APIC_VECTOR_MASK | APIC_SPIV_APIC_ENABLED);
}

static int x2apic_maxlvt(void)
{
        u32 v;

        v = rdmsr(IA32_X2APIC_VER);
        return (((v) >> 16) & 0xFFu);
}

/* From sv6 */
void x2apic_sipi(u32 hwid, u32 addr)
{
        int i;
        u64 accept_status;
        int maxlvt = x2apic_maxlvt();

        wrmsr(IA32_X2APIC_ESR, 0);
        rdmsr(IA32_X2APIC_ESR);

        if (maxlvt > 3)
                wrmsr(IA32_X2APIC_ESR, 0);
        rdmsr(IA32_X2APIC_ESR);

        // "Universal startup algorithm."
        // Send INIT (level-triggered) interrupt to reset other CPU.
        // Asserting INIT
        wrmsr(X2APIC_ICR,
              ((u64)(hwid) << 32) | ICR_TYPE_INIT | ICR_TRIGGER_LEVEL
                      | ICR_LEVEL_ASSERT);
        delay_ms(10);
        // Deasserting INIT
        wrmsr(X2APIC_ICR,
              ((u64)(hwid) << 32) | ICR_TYPE_INIT | ICR_TRIGGER_LEVEL);

        // Send startup IPI (twice!) to enter bootstrap code.
        // Regular hardware is supposed to only accept a STARTUP
        // when it is in the halted state due to an INIT.  So the second
        // should be ignored, but it is part of the official Intel algorithm.
        for (i = 0; i < 2; i++) {
                wrmsr(IA32_X2APIC_ESR, 0);

                // Kick the target chip
                wrmsr(X2APIC_ICR,
                      ((u64)(hwid) << 32) | ICR_TYPE_STARTUP | (addr >> 12));
                // kinfo("Boot addr 0x%lx send 0x%lx\n", addr, addr >> 12);
                delay_ms(1);

                if (maxlvt > 3)
                        wrmsr(IA32_X2APIC_ESR, 0);

                accept_status = (rdmsr(IA32_X2APIC_ESR) & 0xEF);
                // kinfo("Accpet_status %lx\n", accept_status);
                BUG_ON(accept_status);
        }
}

void x2apic_disable_timer(void)
{
        wrmsr(IA32_X2APIC_LVT_TIMER, APIC_LVT_MASKED);
}

void x2apic_enable_timer(void)
{
        wrmsr(IA32_X2APIC_LVT_TIMER, LVT_TIMER_PERIODIC | IRQ_TIMER);
        wrmsr(IA32_X2APIC_INIT_COUNT, cntv_tval);
}

/*
 * Inter-processor Interrupts Sending.
 * Send the interrupt to a specific core.
 */
void x2apic_send_ipi_single(int cpu_id, int interrupt_num)
{
        wrmsr(X2APIC_ICR, (((u64)cpu_id) << 32) | interrupt_num);
}

/* Send the interrupt to other cores except itself */
void x2apic_send_ipi_broadcast(int interrupt_num)
{
        wrmsr(X2APIC_ICR, ICR_DEST_ALL | interrupt_num);
}

/* Send the interrupt to all the cores */
void x2apic_send_ipi_all(int interrupt_num)
{
        wrmsr(X2APIC_ICR, ICR_DEST_ALL_SELF | interrupt_num);
}

void plat_timer_init(void)
{
        x2apic_init_timer();
}

void plat_send_ipi(u32 cpu, u32 ipi)
{
        /*
         * Use x2apic for sending IPI on x86.
         * x2apic_send_ipi_single is used for interrupt a single core.
         */
        x2apic_send_ipi_single(cpu_info[cpu].apic_id, ipi);
}
