#pragma once

#include <common/types.h>

#define VGIC_V2_MAX_LRS (1 << 6)
#define VGIC_MAX_IRQS   1024

struct vgic_cpu_ctrl {
    u32 hcr;
    u32 vmcr;
    u32 misr; /* Saved only */
    u64 eisr; /* Saved only */
    u64 elrsr; /* Saved only */
    u32 apr;
    u32 lr[VGIC_V2_MAX_LRS];
};

struct vgic_cpu {
    u32 nr_lr;
    struct vgic_cpu_ctrl ctrl_regs;
};

struct virt_vm;
struct virt_vcpu;

void vgic_cpu_init(struct vgic_cpu *vgic_cpu);
int vgic_map_gicv_for_vm(struct virt_vm *vm, paddr_t ipa);
int vgic_insert_virq_to_vcpu(struct virt_vcpu *vcpu, int virqno);

/*
 * The following 2 functions are implemented in ASM,
 * and only allowed to be called in run_vcpu_ve (EL2).
 */
void vgic_save_cpu_interface(struct vgic_cpu *vgic_cpu);
void vgic_restore_cpu_interface(struct vgic_cpu *vgic_cpu);
