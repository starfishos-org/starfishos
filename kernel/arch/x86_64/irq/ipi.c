#include "arch/ipi.h"
#include "arch/machine/smp.h"
#include <irq/irq.h>
#include <irq/ipi.h>

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
void arch_handle_ipi(u32 ipi_vector)
{
    switch (ipi_vector) {
    case IPI_TLB_SHOOTDOWN:
        handle_ipi_on_tlb_shootdown();
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
