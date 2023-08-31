#pragma once
#include <common/types.h>

#define ADAGIO_CMD_vm_create                0x0
#define ADAGIO_CMD_vm_destroy               0x1
#define ADAGIO_CMD_vm_enable_feature        0x2
#define ADAGIO_CMD_vm_map_gicv              0x3
#define ADAGIO_CMD_vcpu_create              0x10
#define ADAGIO_CMD_vcpu_run                 0x11
#define ADAGIO_CMD_vcpu_get_one_greg        0x12
#define ADAGIO_CMD_vcpu_set_one_greg        0x13
#define ADAGIO_CMD_vcpu_get_one_sreg        0x14
#define ADAGIO_CMD_vcpu_set_one_sreg        0x15
#define ADAGIO_CMD_vcpu_get_one_hreg        0x16
#define ADAGIO_CMD_vcpu_set_one_hreg        0x17
#define ADAGIO_CMD_vcpu_inject_virq         0x18
#define ADAGIO_CMD_vcpu_ack_pending_virq    0x19
#define ADAGIO_CMD_vcpu_read_pending_virq   0x1a
#define ADAGIO_CMD_vcpu_append_pending_virq 0x1b
#define ADAGIO_CMD_ipa_region_create        0x20
#define ADAGIO_CMD_ipa_region_destroy       0x21
#define ADAGIO_CMD_set_uart_receiver        0x30

int adagio_cmd_vm_create(void);
int adagio_cmd_vm_destroy(int vm_cap);
int adagio_cmd_vm_enable_feature(int vm_cap, u64 feature);
int adagio_cmd_vm_map_gicv(int vm_cap, paddr_t ipa);
int adagio_cmd_vcpu_create(int vm_cap);
/*
 * We do not need a SYS_vcpu_destroy syscall
 * since all vcpus will be destroyed
 * when destroying a vm
 */
int adagio_cmd_vcpu_run(int vcpu_cap);
u64 adagio_cmd_vcpu_get_one_greg(int vcpu_cap, u64 greg_id);
u64 adagio_cmd_vcpu_set_one_greg(int vcpu_cap, u64 greg_id, u64 greg_value);
u64 adagio_cmd_vcpu_get_one_sreg(int vcpu_cap, u64 sreg_id);
u64 adagio_cmd_vcpu_set_one_sreg(int vcpu_cap, u64 sreg_id, u64 sreg_value);
u64 adagio_cmd_vcpu_get_one_hreg(int vcpu_cap, u64 hreg_id);
u64 adagio_cmd_vcpu_set_one_hreg(int vcpu_cap, u64 hreg_id, u64 hreg_value);
u64 adagio_cmd_vcpu_inject_virq(int vcpu_cap, int virqno);
u64 adagio_cmd_vcpu_ack_pending_virq(int vcpu_cap);
u64 adagio_cmd_vcpu_read_pending_virq(int vcpu_cap);
u64 adagio_cmd_vcpu_append_pending_virq(u32 vcpu_cap, int virqno);
u64 adagio_cmd_ipa_region_create(u32 vm_cap, u64 user_config_addr);
int adagio_cmd_ipa_region_destroy(u32 ipa_region_cap);
void adagio_cmd_set_uart_receiver(u32 vm_cap);

/* Syscalls */
u64 sys_virt_dispatch(u64 adagio_syscall_id, u64 param1, u64 param2, u64 param3,
                      u64 param4, u64 param5);
