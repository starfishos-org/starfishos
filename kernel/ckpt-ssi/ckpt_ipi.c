#include <irq/ipi.h>
#include <common/util.h>
#include <ckpt/ckpt.h>
#include <mm/nvm.h>
#include <sched/fpu.h>

#ifdef CHCORE_SSI_SLS
void sys_ipi_stop_all()
{
    u32 target_cpu;
    u32 cpuid = smp_get_cpu_id();

    // printk("sys_ipi_stop_all\n");
    for (target_cpu = 0; target_cpu < PLAT_CPU_NUM; ++target_cpu) {
        if (target_cpu == cpuid)
            continue;
        // wait other cpu to get into ipi
        prepare_ipi_tx(target_cpu);
        start_ipi_tx(target_cpu, IPI_WAIT_IN_KERNEL);
        // kinfo("send ipi to cpu %d\n", target_cpu);
    }
    save_fpu_state(current_thread);
    wait_all_in_kernel(cpuid);
}

void sys_ipi_start_all()
{
    u32 i;
    u32 cpuid = smp_get_cpu_id();
    // printk("sys_ipi_start_all\n");
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if (i == cpuid)
            continue;
        // wait other cpu to get into ipi
        mark_finish_ipi_tx(i);
    }
}

void sys_ipi_test_kernel(int cpuid)
{
    int n = 0;
    while (1) {
        n++;
        if (n % 1000000000 == 0)
            printk("cpu %d\n", cpuid);
    }
}
#endif