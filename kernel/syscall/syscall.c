#include <common/types.h>
#include <common/vars.h>
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
#include <ckpt/ckpt.h>
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/external_sync.h>
#include <ckpt/hybird_mem.h>
#endif /* CHCORE_SLS */
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
#include <ipc/connection.h>
#include <irq/timer.h>
#include <irq/irq.h>
#ifdef CHCORE_KERNEL_VIRT
#include <virt/virt_cmd_dispatcher.h>
#endif /* CHCORE_KERNEL_VIRT */
#include <drivers/pci.h>

#include "syscall_num.h"

#if HOOKING_SYSCALL == ON
void hook_syscall(long n)
{
    if ((n != SYS_putc) && (n != SYS_getc) && (n != SYS_yield)
        && (n != SYS_handle_brk))
        kinfo("[SYSCALL TRACING] hook_syscall num: %ld\n", n);
}
#endif

/* Placeholder for system calls that are not implemented */
void sys_null_placeholder(long arg)
{
    BUG("Invoke non-implemented syscall\n");
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
    extern void disable_fpu_usage();
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

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
void sys_ipi_stop_all();
void sys_ipi_start_all();
void sys_ipi_test_kernel(int cpuid);
void sys_set_dyn_args(u64, u64);
#endif

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
        [SYS_get_poll_remote] = sys_get_poll_remote,
        [SYS_set_poll_remote] = sys_set_poll_remote,
        [SYS_set_excepted_connected_client_num] =
                sys_set_excepted_connected_client_num,
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        [SYS_set_dyn_args] = sys_set_dyn_args,

        /* Checkpoint */
        [SYS_whole_ckpt] = sys_whole_ckpt,
        [SYS_whole_restore] = sys_whole_restore,
        [SYS_whole_ckpt_for_test] = sys_whole_ckpt_for_test,
        [SYS_register_external_ringbuf] = sys_register_external_ringbuf,
        [SYS_ckpt_migrate] = sys_ckpt_migrate,
        [SYS_ckpt_merge_migration] = sys_ckpt_merge_migration,

        /* IPI */
        [SYS_ipi_stop_all] = sys_ipi_stop_all,
        [SYS_ipi_start_all] = sys_ipi_start_all,
        [SYS_ipi_test_kernel] = sys_ipi_test_kernel,

        /* track pf */
        [SYS_track_pf_begin] = sys_track_pf_begin,
        [SYS_track_pf_end] = sys_track_pf_end,

#endif
        [SYS_shutdown] = sys_shutdown,

};
