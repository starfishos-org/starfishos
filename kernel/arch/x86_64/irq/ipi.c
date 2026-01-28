#include "arch/ipi.h"
#include "arch/machine/smp.h"
#include <irq/irq.h>
#include <irq/ipi.h>
#include <mm/vmspace.h>
#include <arch/mm/tlb.h>
#include <arch/mm/page_table.h>
#include <common/kprint.h>
#include <common/macro.h>

void x2apic_send_ipi_single(u32, u32);

void arch_send_ipi(u32 cpu, u32 ipi)
{
    plat_send_ipi(cpu, ipi);
}

/*
 * IPI receiver side:
 * Based on IPI_tx interfaces, ChCore uses the following TLB shootdown
 * protocol between different CPU cores.
 */
extern void flush_local_tlb_opt(vaddr_t start_va, u64 page_cnt, u64 pcid);
extern void clear_history_cpu(struct vmspace *vmspace, u32 cpuid);
void handle_ipi_on_tlb_shootdown(void)
{
    int cpuid;
    u64 start_va;
    u64 page_cnt;
    u64 pcid;
    u64 vmspace;

    cpuid = smp_get_cpu_id();

    start_va = get_ipi_tx_arg(0);
    page_cnt = get_ipi_tx_arg(1);
    pcid = get_ipi_tx_arg(2);
    vmspace = get_ipi_tx_arg(3);

    flush_local_tlb_opt(start_va, page_cnt, pcid);

    /*
     * If the vmspace is running on the current CPU,
     * we should clear the history_cpu records because
     * the vmspace will continue to run after this IPI.
     */
    if (!current_thread || !current_thread->vmspace || 
        (((u64)(current_thread->vmspace) != vmspace) && (vmspace != 0)))
        clear_history_cpu((struct vmspace *)vmspace, cpuid);
}

extern void handle_wait_in_kernel(u32 cpuid);
extern void handle_reset_sched(u32 cpuid);
extern void stop_and_resched(u32 cpuid);

/* Structure for batch TLB flush operations (same as in syscall.c) */
struct tlb_flush_batch_op {
    u64 fault_va;
    u64 len;
    u64 pcid;
    u64 vmspace_ptr;
};

/* Handle batch TLB flush IPI */
void handle_ipi_on_tlb_shootdown_batch(void)
{
    int cpuid;
    u64 ops_buf_ptr;
    u64 ops_count;
    struct tlb_flush_batch_op *ops;
    int i;

    cpuid = smp_get_cpu_id();

    /* Get arguments: ops_buf pointer and ops_count */
    ops_buf_ptr = get_ipi_tx_arg(0);
    ops_count = get_ipi_tx_arg(1);

    if (ops_count == 0 || ops_count > 1024) {
        kwarn("[IPI] Invalid ops_count: %lu\n", ops_count);
        return;
    }

    /* ops_buf_ptr is a kernel address pointing to the operations array */
    ops = (struct tlb_flush_batch_op *)ops_buf_ptr;

    /* Flush TLB for each operation */
    for (i = 0; i < ops_count; i++) {
        struct tlb_flush_batch_op *op = &ops[i];
        flush_local_tlb_opt((vaddr_t)op->fault_va, op->len / PAGE_SIZE, op->pcid);

        /* Clear history_cpu if needed */
        if (op->vmspace_ptr != 0) {
            struct vmspace *vmspace = (struct vmspace *)op->vmspace_ptr;
            if (!current_thread || !current_thread->vmspace || 
                ((u64)(current_thread->vmspace) != op->vmspace_ptr))
                clear_history_cpu(vmspace, cpuid);
        }
    }
}

void arch_handle_ipi(u32 ipi_vector)
{
    switch (ipi_vector) {
    case IPI_TLB_SHOOTDOWN:
        handle_ipi_on_tlb_shootdown();
        break;
    case IPI_TLB_SHOOTDOWN_BATCH:
        handle_ipi_on_tlb_shootdown_batch();
        break;
    case IPI_RESCHED:
        add_pending_resched(smp_get_cpu_id());
        break;
    case IPI_WAIT_IN_KERNEL:
        handle_wait_in_kernel(smp_get_cpu_id());
        break;
    case IPI_RESET_SCHEDULE:
        handle_reset_sched(smp_get_cpu_id());
        /* should never return */
        BUG("handle_reset_sched should never return\n");
    case IPI_STOP_RESCHED:
        return;
    default:
        BUG("Unsupported IPI vector %u\n", ipi_vector);
        break;
    }
}
