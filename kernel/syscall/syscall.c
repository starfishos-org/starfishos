#include <common/types.h>
#include <common/vars.h>
#include <io/uart.h>
#include <mm/uaccess.h>
#include <mm/kmalloc.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <common/kprint.h>
#include <common/debug.h>
#include <object/memory.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <object/recycle.h>
#include <object/object.h>
#include <object/irq.h>
#include <object/user_fault.h>
#include <sched/sched.h>
#include <sched/fpu.h>
#include <ipc/connection.h>
#include <irq/timer.h>
#include <irq/irq.h>
#include <drivers/pci.h>
#include <ipc/futex.h>
#include <mm/shm.h>
#include <mm/page_table_func.h>
#ifdef MULTI_PAGETABLE_ENABLED
#include <mm/vmspace.h>
#endif
#ifdef CHCORE_KERNEL_VIRT
#include <virt/virt_cmd_dispatcher.h>
#endif /* CHCORE_KERNEL_VIRT */

#ifdef CHCORE_SLS
#include <ckpt/ckpt.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/external_sync.h>
#include <ckpt/hybird_mem.h>
#endif /* CHCORE_SLS */

#ifdef CHCORE_SSI_SLS
#include <ckpt/ckpt.h>
#endif

#include "syscall_num.h"

#if HOOKING_SYSCALL == ON
void hook_syscall(long n)
{
    if ((n != SYS_putc) && (n != SYS_getc) && (n != SYS_yield)
        && (n != SYS_handle_brk) && (n != SYS_clock_gettime) && (n != SYS_wait)
        && (n != SYS_ipc_call) && (n != SYS_ipc_return))
        kinfo("[SYSCALL TRACING] hook_syscall num: %ld\n", n);
}
#endif

/* Placeholder for system calls that are not implemented */
void sys_null_placeholder(long arg)
{
    BUG("Invoke non-implemented syscall %lx\n", arg);
}

void sys_putc(char ch)
{
    uart_send((unsigned int)ch);
#if 0 /* Disable graphic output for chcore on r741 */
	if (graphic_putc)
		graphic_putc(ch);
#endif
}

u32 sys_getc(void)
{
    return nb_uart_recv();
}

/* A debugging function which can be used for adding trace points in apps */
void sys_debug_log(long arg)
{
    kinfo("%s: %ld\n", __func__, arg);
}

/* Arch-specific declarations */
void arch_flush_cache(u64, s64, int);
u64 plat_get_current_tick(void);

/* Helper system calls for user-level drivers to use. */
int sys_cache_flush(u64 start, s64 len, int op_type)
{
    arch_flush_cache(start, len, op_type);
    return 0;
}

u64 sys_get_current_tick(void)
{
    return plat_get_current_tick();
}

/* Syscalls for perfromance benchmark */
void sys_perf_start(void)
{
    kdebug("Disable TIMER\n");
    plat_disable_timer();
}

void sys_perf_end(void)
{
    kdebug("Enable TIMER\n");
    plat_enable_timer();
}

void sys_debug_va(u64 va);

extern char shutdown_kernel_stack[PLAT_CPU_NUM][CPU_STACK_SIZE];
void sys_ipi_start_all();
int sys_whole_restore_without_ipi(u64 ckpt_name, u64 name_len);

void handle_shutdown(int reset)
{
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
    /* set CKPT_INITIALIZED to 0 to enable wait_finish_in_kernel */
    u64 old_ckpt_initialized = CKPT_INITIALIZED;
    CKPT_INITIALIZED = 0;
#endif /* CHCORE_SLS */
    extern void sys_ipi_reset_sched_all();
    sys_ipi_reset_sched_all();

    u64 value = 0;
    asm volatile("movq %0, %%gs:%c1"
                 :
                 : "r"(value), "i"(OFFSET_CURRENT_FPU_OWNER)
                 : "memory");
#ifdef CHCORE_SLS
    if (reset) { // reset
        // set nvm flag to 0 means deletes all the ckpts
        old_ckpt_initialized = 0;
        nvm_metadata_reset_crash_flag();
    } else { // restore
        nvm_metadata_set_crash_flag();
    }
#elif defined CHCORE_SSI_SLS
    if (reset) { // reset
        // set nvm flag to 0 means deletes all the ckpts
        old_ckpt_initialized = 0;
        dsm_metadata_reset_crash_flag();
    } else { // restore
        dsm_metadata_set_crash_flag();
    }
#endif
    for (int i = 0; i < PLAT_CPU_NUM; i++) {
        struct per_cpu_info *info;
        info = &cpu_info[i];
        info->fpu_owner = NULL;
    }

    extern void arch_interrupt_init();
    arch_interrupt_init();

    extern void timer_reset();
    timer_reset();

    extern void *get_mb2_mmap();
    if (reset)
        mm_init(NULL, true);
    else
        mm_init((void *)get_mb2_mmap(), false);

    sched_init(&rr);

    extern void init_fpu_owner_locks();
    init_fpu_owner_locks();

#if FPU_SAVING_MODE == LAZY_FPU_MODE
    disable_fpu_usage();
#endif

    extern void reset_user_fault_init(void);
    reset_user_fault_init();

    current_thread = NULL;
#ifdef CHCORE_SLS
    /* Init global metada */
    if (ckpt_metadata_init()) {
        BUG("[ChCore] checkpoint metadata init failed\n");
    }

    init_hybrid_structs();
    kinfo("[ChCore] init_hybrid_structs done\n");

/* Restore checkpoint from nvm metadata */
#ifdef RESTORE_ENABLED
    /* Create initial thread like init-process in Linux */
    if (NVM_IS_CRASH) {
        /* init pre mempcy thread */
        if (!sys_whole_restore_without_ipi(0, 0)) {
            kinfo("[RESTORE] restore from ckpt finished\n");
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
#endif /* CHCORE_SLS */
    create_root_thread();
    kinfo("[ChCore] root thread created\n");

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
#ifdef RESTORE_ENABLED
skip_create_root_thread:
#endif
    /* start all other cores */
    sys_ipi_start_all();

    /* reset ckpt initialized */
    CKPT_INITIALIZED = old_ckpt_initialized;

#endif /* CHCORE_SLS */
    extern void flush_tlb_all();
    flush_tlb_all();

    sched();
    eret_to_thread(switch_context());
    BUG("Should never be here!\n");
}

/* flags related to bypass connection */
int excepted_connected_client_num = __INT_MAX__; // MAX U32
int connected_client_num = 0;
bool poll_remote = true;

int sys_get_poll_remote()
{
    return poll_remote;
}

void sys_set_poll_remote()
{
    connected_client_num++;
    if (connected_client_num >= excepted_connected_client_num) {
        // printk("sys_set_poll_remote to false\n");
        poll_remote = false;
    }
}

void sys_set_excepted_connected_client_num(int expected_num)
{
    excepted_connected_client_num = expected_num;
}

static inline void reset_bypass_connnection_flags()
{
    excepted_connected_client_num = __INT_MAX__;
    connected_client_num = 0;
    poll_remote = true;
}

void sys_shutdown(int flag)
{
    /* switch kernel stack because the mm module will be re-initialized */
    u32 cpuid = smp_get_cpu_id();
    u64 tmp_kernel_stack = (u64)shutdown_kernel_stack[cpuid + 1];
    asm volatile("mov %0, %%rsp" : : "r"(tmp_kernel_stack) :);

    /* reset some flags */
    reset_bypass_connnection_flags();
#ifdef CHCORE_SLS
    extern void clear_external_ringbuf();
    clear_external_ringbuf();
#endif
    /* handle shutdown using the new stack */
    handle_shutdown(flag);
}

mid_t sys_get_machine_id(void)
{
    return CUR_MACHINE_ID;
}

u32 sys_get_machine_cpu_count(void)
{
    return PLAT_CPU_NUM;
}

int sys_memcpy_and_flush_tlb(u64 src_pa, u64 dst_pa, u64 len, u64 fault_va,
                             u64 vmspace_ptr)
{
    /* Convert physical addresses to kernel virtual addresses */
    void *src_va = (void *)phys_to_virt((paddr_t)src_pa);
    void *dst_va = (void *)phys_to_virt((paddr_t)dst_pa);
    struct vmspace *vmspace = (struct vmspace *)vmspace_ptr;
    pte_t *pte = NULL;

    if (!src_va || !dst_va || !vmspace) {
        kwarn("[SYS] Invalid parameters: src_va=%p, dst_va=%p, vmspace=%p\n",
              src_va, dst_va, vmspace);
        return -EINVAL;
    }

    /* Validate len parameter */
    if (len % PAGE_SIZE) {
        kwarn("[SYS] Invalid len: len=%lu\n", len);
        return -EINVAL;
    }

    /* Step 1: Temporarily invalidate PTE (with lock protection) */
    read_lock(&vmspace->vmspace_lock);
    lock(&vmspace->pgtbl_lock);
#ifdef MULTI_PAGETABLE_ENABLED
    query_in_pgtbl(get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID), 
                (vaddr_t)fault_va, NULL, &pte);
#else
    query_in_pgtbl(vmspace->pgtbl, (vaddr_t)fault_va, NULL, &pte);
#endif
    if (!pte) {
        unlock(&vmspace->pgtbl_lock);
        read_unlock(&vmspace->vmspace_lock);
        kwarn("[SYS] query_in_pgtbl failed: pte is NULL for fault_va=0x%lx\n", fault_va);
        return -EINVAL;
    }
    /* Clear the present bit to invalidate the PTE */
    set_migration_entry(pte);
    unlock(&vmspace->pgtbl_lock);
    read_unlock(&vmspace->vmspace_lock);

    /* Step 2: Flush TLB to ensure all CPUs see the NULL mapping */
    extern void flush_tlb_local_and_remote(struct vmspace *vmspace, vaddr_t start_va, size_t len);
    flush_tlb_local_and_remote(vmspace, (vaddr_t)fault_va, (size_t)len);

    /* Step 3: Copy the page (now safe because mapping is NULL) */
    memcpy(dst_va, src_va, (size_t)len);
    kdebug("cpu %d memcpy paddr(%p) to paddr(%p)\n", smp_get_cpu_id(), src_pa, dst_pa);

    // struct page *page = virt_to_page(src_va);
    // kinfo("page: %p, page->pool: %p (type: %d), page->order: %d\n", page, page->pool, page->pool->type, page->order);
    // free_pages(src_va);

    /* Step 4: Remap to dst_pa (must re-acquire lock and re-query pte) */
    read_lock(&vmspace->vmspace_lock);
    lock(&vmspace->pgtbl_lock);
    /* After get_and_clear_pte, PTE is cleared (present=0), so we need to set both pfn and present */
    BUG_ON(!is_migration_entry(pte));
    remap_page_in_pgtbl(pte, dst_pa);  /* Set pfn to dst_pa */
    pte->pte_4K.present = 1;  /* Set present bit to make PTE valid again */
    kdebug("cpu %d remap page(paddr=%p), fault_va=0x%lx\n", smp_get_cpu_id(), dst_pa, fault_va);
    unlock(&vmspace->pgtbl_lock);

    /* Update PMO structure */
    struct vmregion *vmr = find_vmr_for_va(vmspace, (vaddr_t)fault_va);
    if (vmr && vmr->pmo) {
        struct pmobject *pmo = vmr->pmo;
        u64 index = ((vaddr_t)fault_va - vmr->start) / PAGE_SIZE;
        
        /* Only update PMO if it's a radix PMO */
        if (is_radix_pmo(pmo)) {
            commit_page_to_pmo(pmo, index, dst_pa);
        }
    }
    read_unlock(&vmspace->vmspace_lock);

    /* Step 5: Flush TLB again to ensure all CPUs see the new mapping  */
    flush_tlb_local_and_remote(vmspace, (vaddr_t)fault_va, (size_t)len);

    kdebug("memcpy and flush tlb done\n");

    return 0;
}

#ifdef IPC_PERF_ENABLED

#define IPC_PERF_TIME_SIZE 10240
extern volatile bool ipc_perf_enabled;
extern volatile u64 ipc_perf_count_p2;
extern volatile u64 ipc_perf_count_p3;
extern volatile u64 ipc_perf_count_p7;
extern volatile u64 ipc_perf_count_p8;
extern u64 ipc_perf_time_p2[IPC_PERF_TIME_SIZE];
extern u64 ipc_perf_time_p3[IPC_PERF_TIME_SIZE];
extern u64 ipc_perf_time_p7[IPC_PERF_TIME_SIZE];
extern u64 ipc_perf_time_p8[IPC_PERF_TIME_SIZE];

void sys_ipc_perf_start(void)
{
    printk("\033[31mipc_perf_start\033[0m\n");
    ipc_perf_enabled = true;
    ipc_perf_count_p2 = 0;
    ipc_perf_count_p3 = 0;
    for (int i = 0; i < IPC_PERF_TIME_SIZE; i++) {
        ipc_perf_time_p2[i] = 0;
        ipc_perf_time_p3[i] = 0;
        ipc_perf_time_p7[i] = 0;
        ipc_perf_time_p8[i] = 0;
    }
}

void sys_ipc_perf_end(void)
{
    printk("\033[31mipc_perf_end\033[0m\n");
    ipc_perf_enabled = false;
    printk("printing p2 count: %lu\n", ipc_perf_count_p2);
    for (int i = 0; i < ipc_perf_count_p2; i++) {
        printk("%lu ", ipc_perf_time_p2[i]);
    }
    printk("\n");
    printk("printing p3 count: %lu\n", ipc_perf_count_p3);
    for (int i = 0; i < ipc_perf_count_p3; i++) {
        printk("%lu ", ipc_perf_time_p3[i]);
    }
    printk("\n");
    printk("printing p7 count: %lu\n", ipc_perf_count_p7);
    for (int i = 0; i < ipc_perf_count_p7; i++) {
        printk("%lu ", ipc_perf_time_p7[i]);
    }
    printk("\n");
}

#endif

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
void sys_ipi_stop_all();
void sys_ipi_start_all();
void sys_ipi_test_kernel(int cpuid);
#endif

extern void sys_set_dyn_args(u64 hotness, u64 access_interval);
extern int sys_register_external_ringbuf(u64 buffer);

const void *syscall_table[NR_SYSCALL] = {
        [0 ... NR_SYSCALL - 1] = sys_null_placeholder,

        /* Character */
        [SYS_putc] = sys_putc,
        [SYS_getc] = sys_getc,

        /* PMO */
        /* - single */
        [SYS_create_pmo] = sys_create_pmo,
        [SYS_create_device_pmo] = sys_create_device_pmo,
        [SYS_map_pmo] = sys_map_pmo,
        [SYS_unmap_pmo] = sys_unmap_pmo,
        [SYS_write_pmo] = sys_write_pmo,
        [SYS_read_pmo] = sys_read_pmo,
        [SYS_map_with_pmo] = sys_map_with_pmo,
        [SYS_unmap_with_addr] = sys_unmap_with_addr,
        [SYS_revoke_cap] = sys_revoke_cap,
        /* - batch */
        [SYS_create_pmos] = sys_create_pmos,
        [SYS_map_pmos] = sys_map_pmos,
        /* - address translation */
        [SYS_get_pmo_paddr] = sys_get_pmo_paddr,
        [SYS_get_phys_addr] = sys_get_phys_addr,

        /* Capability */
        [SYS_cap_copy_to] = sys_cap_copy_to,
        [SYS_cap_copy_from] = sys_cap_copy_from,
        [SYS_transfer_caps] = sys_transfer_caps,
        [SYS_clone_cap_group] = sys_clone_cap_group,

        /* Multitask */
        /* - create & exit */
        [SYS_create_cap_group] = sys_create_cap_group,
        [SYS_exit_group] = sys_exit_group,
        [SYS_create_thread] = sys_create_thread,
        [SYS_thread_exit] = sys_thread_exit,
        /* - recycle */
        [SYS_register_recycle] = sys_register_recycle,
        [SYS_cap_group_recycle] = sys_cap_group_recycle,
        /* - schedule */
        [SYS_yield] = sys_yield,
        [SYS_set_affinity] = sys_set_affinity,
        [SYS_get_affinity] = sys_get_affinity,

        /* IPC */
        /* - procedure call */
        [SYS_register_server] = sys_register_server,
        [SYS_register_client] = sys_register_client,
        [SYS_ipc_register_cb_return] = sys_ipc_register_cb_return,
        [SYS_ipc_call] = sys_ipc_call,
        [SYS_ipc_return] = sys_ipc_return,
        [SYS_ipc_send_cap] = sys_ipc_send_cap,
        /* - notification */
        [SYS_create_notifc] = sys_create_notifc,
        [SYS_wait] = sys_wait,
        [SYS_notify] = sys_notify,
        /* - futex */
        [SYS_futex] = sys_futex,
        [SYS_set_tid_address] = sys_set_tid_address,

        /* Exception */
        /* - irq */
        [SYS_irq_register] = sys_irq_register,
        [SYS_irq_wait] = sys_irq_wait,
        [SYS_irq_ack] = sys_irq_ack,
#ifdef CHCORE_ENABLE_FMAP
        /* - page fault */
        [SYS_user_fault_register] = sys_user_fault_register,
        [SYS_user_fault_map] = sys_user_fault_map,
#endif

        /* Hardware Access (Privileged Instruction) */
        /* - cache */
        [SYS_cache_flush] = sys_cache_flush,
        /* - timer */
        [SYS_get_current_tick] = sys_get_current_tick,

        /* POSIX */
        /* - time */
        [SYS_clock_gettime] = sys_clock_gettime,
        [SYS_clock_nanosleep] = sys_clock_nanosleep,
        /* - memory */
        [SYS_handle_brk] = sys_handle_brk,
        [SYS_handle_mprotect] = sys_handle_mprotect,

        /* Debug */
        // [SYS_debug_va] = sys_debug_va,
        [SYS_debug_va] = sys_debug_log,
        [SYS_top] = sys_top,
        [SYS_get_free_mem_size] = sys_get_free_mem_size,

        /* Performance Benchmark */
        [SYS_perf_start] = sys_perf_start,
        [SYS_perf_end] = sys_perf_end,
        [SYS_perf_null] = sys_perf_null,

        /* PCIe BUS */
        [SYS_pcie_control] = sys_pcie_control,

/* Virtualization */
#ifdef CHCORE_KERNEL_VIRT
        [SYS_virt_dispatch] = sys_virt_dispatch,
#endif /* CHCORE_KERNEL_VIRT */

        // #ifdef CHCORE_SLS
        [SYS_get_poll_remote] = sys_get_poll_remote,
        [SYS_set_poll_remote] = sys_set_poll_remote,
        [SYS_set_excepted_connected_client_num] =
                sys_set_excepted_connected_client_num,
        [SYS_set_dyn_args] = sys_set_dyn_args,
        [SYS_register_external_ringbuf] = sys_register_external_ringbuf,
// #endif

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        /* Checkpoint */
        [SYS_whole_ckpt] = sys_whole_ckpt,
        [SYS_whole_restore] = sys_whole_restore,
        [SYS_whole_ckpt_for_test] = sys_whole_ckpt_for_test,

        /* IPI */
        [SYS_ipi_stop_all] = sys_ipi_stop_all,
        [SYS_ipi_start_all] = sys_ipi_start_all,
        [SYS_ipi_test_kernel] = sys_ipi_test_kernel,

        /* track pf */
        [SYS_track_pf_begin] = sys_track_pf_begin,
        [SYS_track_pf_end] = sys_track_pf_end,

#endif

#ifdef CHCORE_SSI_SLS
        /* Checkpoint */
        [SYS_cfork_prepare] = sys_cfork_prepare,
        [SYS_cfork_ckpt] = sys_cfork_ckpt,
        [SYS_cfork_restore] = sys_cfork_restore,

        [SYS_ckpt_process] = sys_ckpt_process,
        [SYS_restore_process] = sys_restore_process,
#endif

        [SYS_shutdown] = sys_shutdown,

        [SYS_get_machine_id] = sys_get_machine_id,
        [SYS_get_machine_cpu_count] = sys_get_machine_cpu_count,
        [SYS_register_fs_client] = sys_register_fs_client,
        [SYS_register_fs_server] = sys_register_fs_server,
#ifdef IPC_PERF_ENABLED
        [SYS_ipc_perf_start] = sys_ipc_perf_start,
        [SYS_ipc_perf_end] = sys_ipc_perf_end,
#endif
        [SYS_mmap_shm] = sys_mmap_shm,
        [SYS_memcpy_and_flush_tlb] = sys_memcpy_and_flush_tlb,
};
