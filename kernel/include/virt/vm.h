#pragma once

#include <common/list.h>
#include <virt/vcpu.h>

#define MAX_VCPU_NUM 16
#define MAX_VM_NUM   10000000

struct virt_vm {
    /* A unique number to differentiate different VMs*/
    u64 vm_id;
    /* link all vm in the same list */
    struct list_head vm_list_node;

    /* all vcpus this vm manages */
    struct list_head vcpu_list_head;
    int vcpu_num;

    /* stage 2 mmu struct */
    struct s2mmu *s2mmu;
    /* bitmap to record enabled features */
    u64 enabled_features;
};

void vm_init(struct virt_vm *vm);
void vm_destroy(struct virt_vm *vm);
void init_vm_list(void);

struct virt_vcpu *get_uart_receiver_vcpu();
