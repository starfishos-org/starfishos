#pragma once

#include <common/types.h>

#define GIC_INTID_INVALID    0x3ff
#define GIC_INTID_VIRT_TIMER 0x1b
#define GIC_INTID_VIRT_UART  0x21

struct virt_vcpu;

void vcpu_append_pending_irq(struct virt_vcpu *vcpu, int adagio_irq);
bool vcpu_has_unread_adagio_irq(struct virt_vcpu *vcpu);

void handle_ipi_virt();
void try_send_uart_virq();
