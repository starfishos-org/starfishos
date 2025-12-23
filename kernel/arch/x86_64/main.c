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
#include <io/uart.h>
#include <irq/irq.h>
#include <irq/timer.h>
#include <object/thread.h>
#include <ckpt/ckpt.h>
#include <mm/nvm.h>
#include <ckpt/hybird_mem.h>
#include <ckpt/hot_pages_tracker.h>
#include <dsm/dsm-single.h>
#include <lib/fw_cfg.h>
#include <mm/shm.h>
#include <drivers/ivshmem.h>

/* Global big kernel lock */
struct lock big_kernel_lock;

void run_test(void);
void init_fpu_owner_locks(void);

extern void flush_tlb_all(void);

void main(u64 mbmagic, paddr_t mbaddr)
{
    u32 ret = 0;

    uart_init();
    kdebug("[ChCore] uart init finished\n");

    parse_mb2_info(mbmagic, phys_to_virt(mbaddr));
    kdebug("[ChCore] parse multiboot2 info finished\n");

    /* Init graphic output if avaliable */
    extern void init_console(void);
    init_console();

    /*
     * Multiboot2 will pass the ACPI information
     * and ChCore now retreives the MADT from it for getting APIC info.
     */
    extern void parse_acpi_info(void *info);
    parse_acpi_info((void *)get_mb2_acpi()->rsdp);
    kdebug("[ChCore] parse acpi info finished\n");

    init_per_cpu_info(0); /* should passed from boot? */
    kdebug("[ChCore] per cpu info init finished\n");

    arch_interrupt_init();
    timer_init();
    kdebug("[ChCore] interrupt init finished\n");

    /* Configure the syscall entry */
    arch_syscall_init();
    kdebug("[ChCore] SYSCALL init finished\n");

    mm_init((void *)get_mb2_mmap(), false);
    kdebug("[ChCore] mm init finished\n");

    /* Initialize fw_cfg first to get machine_id before PCI setup */
    fw_cfg_init();
    kdebug("[ChCore] fw_cfg init finished\n");

    /* Configure drivers info */
    pci_setup_devices();
    kdebug("[ChCore] pci setup finished\n");

    ext_mm_init();
    kdebug("[ChCore] external mm init finished\n");

    pci_hostfs_list(NULL);
    kdebug("[ChCore] pci hostfs list finished\n");
    
    /* Register peer_id mapping after dsm_meta is initialized */
    extern void ivshmem_register_peer_id(void);
    kinfo("[ChCore] Registering IVSHMEM peer_id...\n");
    ivshmem_register_peer_id();
    
    /* Configure message processing mode (default: MSI) */
    /* Default to MSI mode - can be changed to IVSHMEM_MSG_MODE_POLLING if needed */
    ivshmem_set_msg_mode(IVSHMEM_MSG_MODE_MSI);
    // ivshmem_set_msg_mode(IVSHMEM_MSG_MODE_POLLING);
    
    /* Start polling thread only if polling mode is enabled */
    /* In MSI mode, messages are processed via MSI-X interrupts */
    if (ivshmem_get_msg_mode() == IVSHMEM_MSG_MODE_POLLING) {
        kinfo("[ChCore] Starting IVSHMEM polling thread (polling mode)...\n");
        ivshmem_start_polling_thread();
    } else {
        kinfo("[ChCore] Using MSI mode for IVSHMEM message processing (no polling thread)\n");
    }
    
    /* Temporarily disable MSI test to avoid memory allocation issues */
    /* TODO: Re-enable after fixing MSI interrupt handling */
    kinfo("[ChCore] Running IVSHMEM MSI communication test...\n");
    extern int ivshmem_test_msi_communication(void);
    ivshmem_test_msi_communication();
    kdebug("[ChCore] MSI communication test finished\n");

    /* Configure CPU features: setting per_core registers */
    arch_cpu_init();
    kdebug("[ChCore] arch cpu init finished\n");

    /* Init big kernel lock */
    ret = lock_init(&big_kernel_lock);
    kdebug("[ChCore] lock init finished\n");
    BUG_ON(ret != 0);

#ifdef CHCORE_KERNEL_RT
    sched_init(&pbrr);
#else
    sched_init(&rr);
#endif
    kdebug("[ChCore] sched init finished\n");

    enable_smp_cores();
    kdebug("[ChCore] boot smp\n");

    init_fpu_owner_locks();
    kdebug("[ChCore] init fpu owner locks\n");

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
    sls_ckpt_init();
#elif defined CHCORE_SSI_SLS
    ssi_ckpt_init();
#endif
    kdebug("[ChCore] ckpt init finished\n");

    shm_init();

    kdebug("[ChCore] shm init finished\n");

    /* Flush all tlbs during boot (kernel uses the lower addresses at boot
     * time) */
    flush_tlb_all();

    /* Create the first user thread */
    create_root_thread();
    kinfo("[ChCore] create initial thread done, machine id: %d\n", CUR_MACHINE_ID);

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
