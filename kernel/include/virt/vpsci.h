#pragma once

#include <common/types.h>
#include <virt/vm.h>

#define PRIMARY_VCPU_ID 0x0

/* VPSCI define according to SMC32 calling convention */
#define VPSCI_SMC32_VERSION           0x84000000
#define VPSCI_SMC32_CPU_ON            0x84000003
#define VPSCI_SMC32_MIGRATE_INFO_TYPE 0x84000006

#define VPSCI_SMC64_CPU_ON 0xc4000003

#define VPSCI_SUCCESS 0

u64 do_psci_on(struct virt_vm *vm, u64 target_cpu_id, u64 entry_point,
               u64 context_id);
