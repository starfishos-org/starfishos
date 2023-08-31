#pragma once

#include <machine.h>
#include <common/list.h>
#include <common/types.h>
#include <common/lock.h>
#include <virt/vm.h>
#include <virt/vtimer.h>
#ifdef FEAT_VGIC
#include <virt/vgic.h>
#endif
#include <arch/virt/arch_regs.h>

#define EXIT_REASON_UNKNOWN       1
#define EXIT_REASON_NOMAPPING     2
#define EXIT_REASON_EACCES        3
#define EXIT_REASON_ADAGIO_IRQ    4
#define EXIT_REASON_RERUN         5
#define EXIT_REASON_FINISH_VMTEST 256

#define ADAGIO_IRQ_BUF_SIZE 32

struct vcpu_ctx {
	struct gp_regs gp_regs;
	struct sys_regs sys_regs;
	struct hyp_regs hyp_regs;
	struct vtimer vtimer;

#ifdef FEAT_VGIC
	struct vgic_cpu vgic_cpu;
#endif
};

struct pending_adagio_irq {
	struct lock lock;
	u8 irq_buf[ADAGIO_IRQ_BUF_SIZE];
	u32 irq_buf_append_idx;
	u32 irq_buf_read_idx;
	u32 irq_buf_clear_idx;
};

struct virt_vcpu {
	/* Point to vm struct the vcpu belongs to */
	struct virt_vm *vm;
	/* Store vcpu register states */
	struct vcpu_ctx *vcpu_ctx;

	struct pending_adagio_irq *pending_adagio_irq;
	/* link all vcpu in the same vm */
	struct list_head vcpu_list_node;

	int current_phys_cpuid;

	/*
	 * TODO:
	 * use cpu_state enum rather than a single variable to describe cpu state,
	 * which should contain VCPU_INIT, VCPU_ON, VCPU_OFF at least.
	 */
	bool plugged;
};

struct host_cpu_ctx {
	struct gp_regs gp_regs;
	struct sys_regs sys_regs;
	struct hyp_regs hyp_regs;
};

struct percpu_hyp_state {
	struct virt_vcpu *current_vcpu;
	struct host_cpu_ctx *host_cpu_ctx;
};

extern struct percpu_hyp_state percpu_hyp_states[PLAT_CPU_NUM];

#define get_current_hyp_state() (percpu_hyp_states[smp_get_cpu_id()])
#define get_current_vcpu()      (get_current_hyp_state().current_vcpu)

#define DEFINE(sym, val) \
	asm volatile("\n.ascii \"->" #sym " %0 " #val "\"" : : "i"(val))

#define asmoffsetof(TYPE, MEMBER) ((u64) & ((TYPE *)0)->MEMBER)

#define ADAGIO_GPREG_PREFIX    0x1000000000000000
#define ADAGIO_SYSREG_PREFIX   0x2000000000000000
#define ADAGIO_HYPREG_PREFIX   0x3000000000000000
#define ADAGIO_REG_PREFIX_MASK 0xf000000000000000
static inline u64 extract_adagio_reg_offset_from_id(u64 reg_id)
{
	return reg_id & ~ADAGIO_REG_PREFIX_MASK;
}

int vcpu_run(struct virt_vcpu *vcpu);
void vcpu_init(struct virt_vcpu *vcpu, struct virt_vm *vm);
int add_vcpu_to_vm(struct virt_vcpu *vcpu, struct virt_vm *vm);
int delete_vcpu_from_vm(struct virt_vcpu *vcpu, struct virt_vm *vm);
void vcpu_destroy(struct virt_vcpu *vcpu);

extern u64 enter_guest(void);
extern void save_sys_states(struct sys_regs *regs);
extern void restore_sys_states(struct sys_regs *regs);
