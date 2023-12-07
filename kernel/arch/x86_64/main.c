#include <common/types.h>
#include <common/kprint.h>
#include <common/lock.h>
#include <mm/mm.h>
#include <sched/sched.h>
#include <sched/fpu.h>
#include <arch/machine/smp.h>
#include <arch/machine/machine.h>
#include <arch/drivers/multiboot2.h>
#include <drivers/pci.h>
#include <drivers/cxl.h>
#include <io/uart.h>
#include <irq/irq.h>
#include <irq/timer.h>
#include <object/thread.h>
#include <ckpt/ckpt.h>
#include <mm/nvm.h>
#include <ckpt/hybird_mem.h>
#include <ckpt/hot_pages_tracker.h>

/* Global big kernel lock */
struct lock big_kernel_lock;

void run_test(void);
void init_fpu_owner_locks(void);

extern void flush_tlb_all(void);

void main(u64 mbmagic, paddr_t mbaddr)
{
        u32 ret = 0;

        uart_init();
        kinfo("[ChCore] uart init finished\n");

        parse_mb2_info(mbmagic, phys_to_virt(mbaddr));
        kinfo("[ChCore] parse multiboot2 info finished\n");

        /* Init graphic output if avaliable */
        extern void init_console(void);
        init_console();

        /*
         * Multiboot2 will pass the ACPI information
         * and ChCore now retreives the MADT from it for getting APIC info.
         */
        extern void parse_acpi_info(void *info);
        parse_acpi_info((void *)get_mb2_acpi()->rsdp);
        kinfo("[ChCore] parse acpi info finished\n");

        init_per_cpu_info(0); /* should passed from boot? */
        kinfo("[ChCore] per cpu info init finished\n");

        arch_interrupt_init();
        timer_init();
        kinfo("[ChCore] interrupt init finished\n");

        /* Configure the syscall entry */
        arch_syscall_init();
        kinfo("[ChCore] SYSCALL init finished\n");

        mm_init((void *)get_mb2_mmap(), false);
        kinfo("[ChCore] mm init finished\n");

        /* Configure drivers info */
        arch_pci_mmcfg_init();
        pci_setup_devices();
        kinfo("[ChCore] pci devices setup finished\n");

        cxl_setup_devices();
        kinfo("[ChCore] cxl devices setup finished\n");

        /* Configure CPU features: setting per_core registers */
        arch_cpu_init();

        /* Init big kernel lock */
        ret = lock_init(&big_kernel_lock);
        kinfo("[ChCore] lock init finished\n");
        BUG_ON(ret != 0);

#ifdef CHCORE_KERNEL_RT
        sched_init(&pbrr);
#else
        sched_init(&rr);
#endif
        kinfo("[ChCore] sched init finished\n");

        enable_smp_cores();
        kinfo("[ChCore] boot smp\n");

        init_fpu_owner_locks();
        kinfo("[ChCore] init fpu owner locks\n");

        /* Test should be done when IRQ is not enabled */
#ifdef CHCORE_KERNEL_TEST
        kinfo("[ChCore] kernel tests start\n");
        run_test();
        kinfo("[ChCore] kernel tests done\n");
#endif /* CHCORE_KERNEL_TEST */

#if FPU_SAVING_MODE == LAZY_FPU_MODE
        disable_fpu_usage();
#endif

#ifdef CHCORE_SLS
        /* Init global metada */
        if (ckpt_metadata_init()) {
                BUG("[ChCore] checkpoint metadata init failed\n");
        }
        kinfo("[ChCore] ckpt_metadata_init done\n");

        init_hybrid_structs();
        kinfo("[ChCore] init_hybrid_structs done\n");

        /* Restore checkpoint from nvm metadata */
#ifdef RESTORE_ENABLED
        /* Create initial thread like init-process in Linux */
        if (NVM_IS_CRASH) {
                /* init pre mempcy thread */
                if (!sys_whole_restore(0, 0)) {
                        kinfo("[RESTORE] restore from ckpt\n");
                        goto skip_create_root_thread;
                } else {
                        kinfo("[ChCore] sys_whole_restore error\n");
                }
        }
        /* After all finish, set crash flag  */
        nvm_metadata_set_crash_flag();
        kinfo("[ChCore] nvm_metadata_set_crash_flag done\n");
#else
        nvm_metadata_reset_crash_flag();
#endif
#endif
        /* Flush all tlbs during boot (kernel uses the lower addresses at boot
         * time) */
        flush_tlb_all();

        /* Create the first user thread */
        create_root_thread();
        kinfo("[ChCore] create initial thread done\n");

#ifdef RESTORE_ENABLED
skip_create_root_thread:
#endif
        sched();
        eret_to_thread(switch_context());
        BUG("Should never be here!\n");
}

/* For booting smp cores */
void secondary_start(u32 cpuid)
{
        arch_interrupt_init_per_cpu();
        init_per_cpu_info(cpuid);
        timer_init();

        /* Configure the syscall entry */
        arch_syscall_init();
        /* Configure CPU features: setting per_core registers */
        arch_cpu_init();

        /* Test should be done when IRQ is not enabled */
#ifdef CHCORE_KERNEL_TEST
        run_test();
#endif /* CHCORE_KERNEL_TEST */

#if FPU_SAVING_MODE == LAZY_FPU_MODE
        disable_fpu_usage();
#endif

        /* Flush all tlbs during boot (kernel uses the lower addresses at boot
         * time) */
        flush_tlb_all();

        /* Run the scheduler on the current CPU core */
        sched();
        eret_to_thread(switch_context());
        BUG("Should never be here!\n");
}
