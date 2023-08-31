#pragma once

#include <common/types.h>
#include <irq/ipi.h>

#define T_DE    0		// divide error
#define T_DB    1		// debug exception
#define T_NMI   2		// non-maskable interrupt
#define T_BP    3		// breakpoint
#define T_OF    4		// overflow
#define T_BR    5		// bounds range check
#define T_UD    6		// undefined opcode
#define T_NM    7		// device not available
#define T_DF    8		// double fault
#define T_CSO   9		// coprocessor segment overrun
#define T_TS    10		// invalid task switch segment
#define T_NP    11		// segment not present
#define T_SS    12		// stack exception
#define T_GP    13		// general protection fault
#define T_PF    14		// page fault
#define T_RES   15		// reserved
#define T_MF    16		// floating point error
#define T_AC    17		// alignment check
#define T_MC    18		// machine check
#define T_XM    19		// SIMD floating point error
#define T_VE    20		// virtualization exception

#define IRQ_TIMER 32

/* IRQ number for IPI (defined in ChCore) */
#define IRQ_IPI_TLB     IPI_TLB_SHOOTDOWN
#define IRQ_IPI_RESCHED IPI_RESCHED
#define IRQ_IPI_WAIT_IN_KERNEL IPI_WAIT_IN_KERNEL
#define IRQ_IPI_RESET_SCHED IPI_RESET_SCHEDULE


#define T_NUM   256             // use all 256 entry

/* Gate descriptors for interrupts and traps, see figure6-7 */
struct gate_desc {
	unsigned gd_off_15_0 : 16;   // low 16 bits of offset in segment
	unsigned gd_sel : 16;        // segment selector
	unsigned gd_args : 5;        // args, 0 for interrupt/trap gates
	unsigned gd_rsv1 : 3;        // reserved(should be zero I guess)
	unsigned gd_type : 4;        // type
	unsigned gd_s : 1;           // must be 0 (system)
	unsigned gd_dpl : 2;         // descriptor(meaning new) privilege level
	unsigned gd_p : 1;           // present
	unsigned gd_off_31_16 : 16;  // middle 16 bits of offset in segment
        unsigned gd_off_63_32 : 32;  // high 32 bits of offset in segment
        unsigned gd_reserved : 32;   // reserved
} __attribute__ ((aligned(16)));

struct pseudo_desc
{
        u16 limit;
        u64 base;
} __attribute__((packed, aligned(16)));

#define GT_CALL_GATE    12      /* 64-bit Call Gate */
#define GT_INTR_GATE    14      /* 64-bit Interrupt Gate */
#define GT_TRAP_GATE    15      /* 64-bit Trap Gate */

/*
 * Set up a normal interrupt/trap gate descriptor.
 * - type: Gate type
 * - sel:  Code segment selector for interrupt/trap handler
 * - off:  Offset in code segment for interrupt/trap handler
 * - dpl:  Descriptor Privilege Level -
 *	   the privilege level required for software to invoke
 *	   this interrupt/trap gate explicitly using an int instruction.
 */
#define set_gate(gate, type, sel, off, dpl)			\
{								\
	(gate).gd_off_15_0 = (u64) (off) & 0xffff;		\
	(gate).gd_sel = (sel);					\
	(gate).gd_args = 0;					\
	(gate).gd_rsv1 = 0;					\
	(gate).gd_type = (type) & 0xf;                          \
	(gate).gd_s = 0;					\
	(gate).gd_dpl = (dpl);					\
	(gate).gd_p = 1;					\
	(gate).gd_off_31_16 = ((u64) (off) >> 16) & 0xffff;	\
        (gate).gd_off_63_32 = ((u64) (off) >> 32);              \
        (gate).gd_reserved = 0;                                 \
}

/* Return to context store in sp in two different cases */
void eret_to_thread_through_sys_exit(u64 sp);
void eret_to_thread_through_trap_exit(u64 sp);
/* IPC exit declartion */
void eret_to_thread_through_ipc_exit(u64 sp);
/* Fast syscall entry */
void sys_entry();
/* IDT entries defined in irq_entry.S */
extern u64 idt_entry[];

/* Exception Handler */
void do_page_fault(u64 errorcode, u64 fault_ins_addr);
