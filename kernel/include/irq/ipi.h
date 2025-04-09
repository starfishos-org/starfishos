#pragma once

#include <irq/irq.h>
#include <common/types.h>
#include <common/lock.h>
#include <arch/ipi.h>

void arch_send_ipi(u32 cpu, u32 ipi);

/* 7 u64 arg and 2 u32 (start/finish, vector) occupy one cacheline */
#define IPI_DATA_ARG_NUM (7)

void init_ipi_data(void);

/* IPI interfaces for achieving cross-core communication */

/* Sender side */
void prepare_ipi_tx(u32 target_cpu);
void set_ipi_tx_arg(u32 target_cpu, u32 arg_index, u64 val);
void start_ipi_tx(u32 target_cpu, u32 ipi_vector);
void wait_finish_ipi_tx(u32 target_cpu);
void send_ipi(u32 target_cpu, u32 ipi_vector);

/* Receiver side */
u64 get_ipi_tx_arg(u32 arg_index);
void handle_ipi(void);
void arch_handle_ipi(u32 ipi_vector);

/* For global stop */
void wait_finish_in_kernel(u32 target_cpu);
void wait_all_in_kernel(u32 except_cpu);
void mark_in_kernel_ipi_tx(u32 target_cpu);
void mark_finish_ipi_tx(u32 target_cpu);

/* stop and start a cpu */
bool is_cpu_stop_in_kernel(u32 cpuid);
void sys_ipi_start_cpu(u32 cpuid);
void sys_ipi_stop_cpu(u32 cpuid);
