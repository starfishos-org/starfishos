#include <mm/mm.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/errno.h>
#include <irq/ipi.h>
#include <arch/mm/page_table.h>
#include <arch/mm/tlb.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#include <drivers/ivshmem.h>
#include <mm/shm.h>
#include <common/mem_sync.h>
#include <arch/sync.h>
#endif

/* Operations that invalidate TLBs and Paging-Structure Caches */

/*
 * flush_tlb(void *addr): flush tlb for one address
 *
 * invlpg:
 *       - flush corresponding tlb with current PCID
 *       - flush global tlb with the physical page number, regardless of PCID
 *       - flush all paging-structure cacahes with current PCID
 */
void flush_single_tlb(vaddr_t addr)
{
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/*
 * INVPCID: 4 types as follows:
 */
#define INVPCID_TYPE_INDIV_ADDR      0
#define INVPCID_TYPE_SINGLE_CTXT     1
#define INVPCID_TYPE_ALL_INCL_GLOBAL 2
#define INVPCID_TYPE_ALL_NON_GLOBAL  3

void __invpcid(u64 pcid, u64 addr, u64 type)
{
    struct {
        u64 d[2];
    } __attribute__((aligned(16))) desc = {{pcid, addr}};

    /*
     * The memory clobber is because the whole point is to invalidate
     * stale TLB entries and, especially if we're flushing global
     * mappings, we don't want the compiler to reorder any subsequent
     * memory accesses before the TLB flush.
     *
     * The hex opcode is invpcid (%ecx), %eax in 32-bit mode and
     * invpcid (%rcx), %rax in long mode.
     */
    asm volatile(".byte 0x66, 0x0f, 0x38, 0x82, 0x01"
                 :
                 : "m"(desc), "a"(type), "c"(&desc)
                 : "memory");
}

/* Flush all mappings for a given pcid and addr, not including globals. */
// static inline
void invpcid_flush_one(u64 pcid, u64 addr)
{
    __invpcid(pcid, addr, INVPCID_TYPE_INDIV_ADDR);
}

/* Flush all mappings for a given PCID, not including globals. */
// static inline
void invpcid_flush_single_context(u64 pcid)
{
    __invpcid(pcid, 0, INVPCID_TYPE_SINGLE_CTXT);
}

/* Flush all mappings, including globals, for all PCIDs. */
// static inline
void invpcid_flush_all(void)
{
    __invpcid(0, 0, INVPCID_TYPE_ALL_INCL_GLOBAL);
}

/* Flush all mappings for all PCIDs except globals. */
// static inline
void invpcid_flush_all_nonglobals(void)
{
    __invpcid(0, 0, INVPCID_TYPE_ALL_NON_GLOBAL);
}

/*
 * x86_64 have several other options to flush all tlb.
 */

/*
 * MOV to CR3 when CR4.PCIDE = 1:
 *     - if bit 63 of the instruction source operand is 0: flush TLB with the
 * PCID
 *     - if bit 63 is 1: do not flush TLB
 */

#ifdef CHCORE
void flush_tlb_all(void)
{
#if 1
    invpcid_flush_all();
#else
    /* If PCID is not supported, flush TLB with writing CR3 */
    paddr_t pgtbl;

    pgtbl = get_page_table();
    /* clear the highest bit: flush TLB of current PCID */
    pgtbl &= ~(1UL << 63);
    // printk("pgtbl: 0x%lx\n", pgtbl);
    set_page_table(pgtbl);
#endif
}
#endif

/*
 * IPI sender side:
 * Based on IPI_tx interfaces, ChCore uses the following TLB shootdown
 * protocol between different CPU cores.
 */
#if 0
static void flush_remote_tlb_with_ipi(u32 target_cpu, vaddr_t start_va,
                                      u64 page_cnt, u64 pcid, u64 vmspace)
{
    /* IPI_tx: step-1 */
    prepare_ipi_tx(target_cpu);

    /* IPI_tx: step-2 */
    /* set the first argument */
    set_ipi_tx_arg(target_cpu, 0, start_va);
    /* set the second argument */
    set_ipi_tx_arg(target_cpu, 1, page_cnt);
    /* set the third argument */
    set_ipi_tx_arg(target_cpu, 2, pcid);
    /* set the fourth argument */
    set_ipi_tx_arg(target_cpu, 3, vmspace);

    /* IPI_tx: step-3 */
    start_ipi_tx(target_cpu, IPI_TLB_SHOOTDOWN);

    /* IPI_tx: step-4 */
    wait_finish_ipi_tx(target_cpu);
}
#endif

/* Currently, ChCore uses a simple policy for choosing how to flush TLB */
// TODO: refer to Linux on how to flush TLB (for better performance)
#define TLB_SHOOTDOWN_THRESHOLD 2
void flush_local_tlb_opt(vaddr_t start_va, u64 page_cnt, u64 pcid)
{
    if (page_cnt > TLB_SHOOTDOWN_THRESHOLD) {
        /* Flush all the TLBs of the PCID */
        invpcid_flush_single_context(pcid);
    } else {
        u64 i;
        u64 addr;

        /* Flush each TLB entry one-by-one */
        addr = start_va;
        for (i = 0; i < page_cnt; ++i) {
            invpcid_flush_one(pcid, addr);
            addr += PAGE_SIZE;
        }
    }
}

/*
 * This function is responsible for flushing the TLBs (with the
 * corresponding VA range provided) on each necessary CPU.
 */
void flush_tlb_local_and_remote(struct vmspace *vmspace, vaddr_t start_va,
                                size_t len)
{
    /* page_cnt, i.e., TLB_entry_cnt */
    u64 page_cnt;
    u64 pcid;
    u32 cpuid;
    u32 i;
    u32 target_count = 0;
    u8 cpu_mask[PLAT_CPU_NUM] = {0};

    if (unlikely(len < PAGE_SIZE))
        kwarn("func: %s. len (%p) < PAGE_SIZE\n", __func__, len);

    if (len == 0)
        return;

    len = ROUND_UP(len, PAGE_SIZE);
    page_cnt = len / PAGE_SIZE;

#ifdef MULTI_PAGETABLE_ENABLED
    pcid = get_pcid(get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID));
#else
    pcid = get_pcid(vmspace->pgtbl);
#endif

    cpuid = smp_get_cpu_id();

    /* Flush remote TLBs in parallel */
    /* Step 1.0: Prepare and send IPIs to all target CPUs without waiting */
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if ((i != cpuid) && (vmspace->history_cpus[i] == 1)) {
            /* IPI_tx: step-1 */
            prepare_ipi_tx(i);
            /* IPI_tx: step-2 */
            set_ipi_tx_arg(i, 0, start_va);
            set_ipi_tx_arg(i, 1, page_cnt);
            set_ipi_tx_arg(i, 2, pcid);
            set_ipi_tx_arg(i, 3, (u64)vmspace);
            /* IPI_tx: step-3 */
            start_ipi_tx(i, IPI_TLB_SHOOTDOWN);
            cpu_mask[i] = 1;
            target_count++;
        }
    }

    /* Step 1.1: If necessary, flush local TLBs */
    if (vmspace->history_cpus[cpuid] == 1) {
        flush_local_tlb_opt(start_va, page_cnt, pcid);
    }

    /* Step 2: Wait for all IPIs to finish and unlock */
    if (target_count > 0) {
        wait_ipi_finish_mask(cpuid, cpu_mask, target_count);
    }
}


/* Structure for batch TLB flush operations */
struct tlb_flush_batch_op {
    u64 fault_va;
    u64 len;
    u64 pcid;
    u64 vmspace_ptr;
};

/*
 * Flush TLBs for batch operations on all CPUs using batch IPI.
 * This function prepares a batch TLB flush operation list and sends
 * a single IPI to each target CPU with the operations buffer pointer.
 */
void flush_tlbs_batch_on_all_cpus(struct tlb_flush_batch_op *ops, u64 ops_count)
{
    u32 cpuid;
    u32 i;
    u32 target_count = 0;
    u8 cpu_mask[PLAT_CPU_NUM] = {0};

    if (ops_count == 0 || ops_count > 1024) {
        kwarn("func: %s. Invalid ops_count: %lu\n", __func__, ops_count);
        return;
    }

    cpuid = smp_get_cpu_id();

    /* Flush remote TLBs in parallel */
    /* Step 1.0: Prepare and send batch IPIs to all target CPUs without waiting */
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if (i != cpuid) {
            /* IPI_tx: step-1 */
            prepare_ipi_tx(i);
            /* IPI_tx: step-2 */
            /* arg0: ops buffer pointer (kernel address) */
            set_ipi_tx_arg(i, 0, (u64)ops);
            /* arg1: ops_count */
            set_ipi_tx_arg(i, 1, ops_count);
            /* IPI_tx: step-3 */
            start_ipi_tx(i, IPI_TLB_SHOOTDOWN_BATCH);
            cpu_mask[i] = 1;
            target_count++;
        }
    }

    /* Step 1.1: Flush local TLBs */
    for (i = 0; i < ops_count; i++) {
        struct tlb_flush_batch_op *op = &ops[i];
        u64 page_cnt = op->len / PAGE_SIZE;
        flush_local_tlb_opt((vaddr_t)op->fault_va, page_cnt, op->pcid);
    }

    /* Step 2: Wait for all IPIs to finish and unlock */
    if (target_count > 0) {
        wait_ipi_finish_mask(cpuid, cpu_mask, target_count);
    }
}

/*
 * This function is responsible for flushing the TLBs (with the
 * corresponding VA range provided) on all CPUs (but not only the necessary ones)
 */
 void flush_tlbs_on_all_cpus(struct vmspace *vmspace, vaddr_t start_va,
    size_t len)
{
    /* page_cnt, i.e., TLB_entry_cnt */
    u64 page_cnt;
    u64 pcid;
    u32 cpuid;
    u32 i;
    u32 target_count = 0;
    u8 cpu_mask[PLAT_CPU_NUM] = {0};

    if (unlikely(len < PAGE_SIZE))
        kwarn("func: %s. len (%p) < PAGE_SIZE\n", __func__, len);

    if (len == 0)
        return;

    len = ROUND_UP(len, PAGE_SIZE);
    page_cnt = len / PAGE_SIZE;

#ifdef MULTI_PAGETABLE_ENABLED
    pcid = get_pcid(get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID));
#else
    pcid = get_pcid(vmspace->pgtbl);
#endif

    cpuid = smp_get_cpu_id();

    /* Flush remote TLBs in parallel */
    /* Step 1.0: Prepare and send IPIs to all target CPUs without waiting */
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        // if (i != cpuid && vmspace->history_cpus[i] == 1) {
        if (i != cpuid) {
            /* IPI_tx: step-1 */
            prepare_ipi_tx(i);
            /* IPI_tx: step-2 */
            set_ipi_tx_arg(i, 0, start_va);
            set_ipi_tx_arg(i, 1, page_cnt);
            set_ipi_tx_arg(i, 2, pcid);
            set_ipi_tx_arg(i, 3, (u64)vmspace);
            /* IPI_tx: step-3 */
            start_ipi_tx(i, IPI_TLB_SHOOTDOWN);
            cpu_mask[i] = 1;
            target_count++;
        }
    }

    /* Step 1.1: If necessary, flush local TLBs */
    // if (vmspace->history_cpus[cpuid] == 1) {
        flush_local_tlb_opt(start_va, page_cnt, pcid);
    // }

    /* Step 2: Wait for all IPIs to finish and unlock */
    if (target_count > 0) {
        wait_ipi_finish_mask(cpuid, cpu_mask, target_count);
    }
}

/*
 * This function is responsible for flushing the TLBs (with the
 * corresponding PCID provided) on each necessary CPU.
 */
static void flush_tlb_by_pcid_global(struct vmspace *vmspace)
{
    /* The dummy_va will not be used. */
    u64 dummy_va = 0;
    /* Set page_cnt to inifite for flushing all the TLBs of the PCID. */
    u64 page_cnt = -1;
    /* The dummy_vmspace will not be used. */
    u64 dummy_vmspace = 0;
    u64 pcid = vmspace->pcid;
    u32 cpuid;
    u32 i;
    u32 target_count = 0;
    u8 cpu_mask[PLAT_CPU_NUM] = {0};

    /* Flush local TLBs */
    flush_local_tlb_opt(dummy_va, page_cnt, pcid);

    cpuid = smp_get_cpu_id();
    /* Flush remote TLBs in parallel */
    /* Step 1: Prepare and send IPIs to all target CPUs without waiting */
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if ((i != cpuid) && (vmspace->history_cpus[i] == 1)) {
            /* IPI_tx: step-1 */
            prepare_ipi_tx(i);
            /* IPI_tx: step-2 */
            set_ipi_tx_arg(i, 0, dummy_va);
            set_ipi_tx_arg(i, 1, page_cnt);
            set_ipi_tx_arg(i, 2, pcid);
            set_ipi_tx_arg(i, 3, dummy_vmspace);
            /* IPI_tx: step-3 */
            start_ipi_tx(i, IPI_TLB_SHOOTDOWN);
            cpu_mask[i] = 1;
            target_count++;
        }
    }

    /* Step 2: Wait for all IPIs to finish and unlock */
    if (target_count > 0) {
        wait_ipi_finish_mask(cpuid, cpu_mask, target_count);
    }
}

void flush_tlb_of_vmspace(struct vmspace *vmspace)
{
    flush_tlb_by_pcid_global(vmspace);
}

/*
 * Drop every TLB entry tagged with @pcid on every CPU.
 *
 * ChCore loads CR3 with bit 63 set (see set_page_table), so a page table
 * switch never flushes the incoming PCID.  procmgr hands out PCIDs from a
 * recycling id_manager, and every process shares the same virtual layout
 * (code at 0x400000, stack at 0x500000000000, heap at 0x600000000000), so a
 * PCID handed to a new process can still have live entries from the dead
 * process that used it -- entries that translate without faulting, giving the
 * new process silent reads of the dead one's pages.
 *
 * flush_tlb_by_pcid_global() only visits vmspace->history_cpus, which is
 * empty for a vmspace that has not run yet.  A newly assigned PCID therefore
 * has to be cleared everywhere before its first use.
 */
void flush_tlb_by_pcid_all_cpus(u64 pcid)
{
    /* The dummy args are unused when flushing a whole PCID context. */
    u64 dummy_va = 0;
    u64 page_cnt = -1;
    u64 dummy_vmspace = 0;
    u32 cpuid;
    u32 i;
    u32 target_count = 0;
    u8 cpu_mask[PLAT_CPU_NUM] = {0};

    flush_local_tlb_opt(dummy_va, page_cnt, pcid);

    cpuid = smp_get_cpu_id();
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if (i == cpuid)
            continue;
        /* IPI_tx: step-1 */
        prepare_ipi_tx(i);
        /* IPI_tx: step-2 */
        set_ipi_tx_arg(i, 0, dummy_va);
        set_ipi_tx_arg(i, 1, page_cnt);
        set_ipi_tx_arg(i, 2, pcid);
        set_ipi_tx_arg(i, 3, dummy_vmspace);
        /* IPI_tx: step-3 */
        start_ipi_tx(i, IPI_TLB_SHOOTDOWN);
        cpu_mask[i] = 1;
        target_count++;
    }

    if (target_count > 0) {
        wait_ipi_finish_mask(cpuid, cpu_mask, target_count);
    }
}

void flush_tlbs(struct vmspace *vmspace, vaddr_t start_va, size_t len)
{
    flush_tlb_local_and_remote(vmspace, start_va, len);
}

#ifdef MULTI_PAGETABLE_ENABLED
#include <common/lock.h>

/* Flush TLB only for CPUs belonging to a specific machine.
 * Returns 0 once that machine has acknowledged, -ETIMEDOUT otherwise. */
int flush_tlb_on_remote_machine(struct vmspace *vmspace, mid_t machine_id,
                                vaddr_t start_va, size_t len)
{
    mid_t my_id = CUR_MACHINE_ID;

    if (!dsm_meta || machine_id >= CLUSTER_MACHINE_NUM || machine_id == my_id)
        return 0;

    /* Prepare message in target machine's slot (with lock protection) */
    lock(&dsm_meta->msi_test_msg[machine_id].msg_lock);
    dsm_meta->msi_test_msg[machine_id].msg_from = my_id;
    dsm_meta->msi_test_msg[machine_id].msg_type = MSI_MSG_TYPE_TLB_FLUSH;
    dsm_meta->msi_test_msg[machine_id].reply_received = 0;
    dsm_meta->msi_test_msg[machine_id].reply_from = 0xFFFFFFFF;
    dsm_meta->msi_test_msg[machine_id].tlb_start_va = start_va;
    dsm_meta->msi_test_msg[machine_id].tlb_len = len;
    dsm_meta->msi_test_msg[machine_id].tlb_vmspace = (u64)vmspace;
    unlock(&dsm_meta->msi_test_msg[machine_id].msg_lock);

    /* Clear our own slot's reply flag before sending request */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 0;
    dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);

    /* Send MSI interrupt */
    extern enum ivshmem_msg_mode ivshmem_get_msg_mode(void);
    if (ivshmem_get_msg_mode() == IVSHMEM_MSG_MODE_MSI) {
        /* Send MSI to notify remote machine */
        extern int ivshmem_send_msi(mid_t target_machine_id, u16 vector);
        ivshmem_send_msi(machine_id, 0); /* Use vector 0 for TLB flush */
    }
    /* Note: We still poll for reply even in MSI mode because we're in a page
     * fault handler and cannot wait for interrupts. The MSI will trigger
     * processing on the remote machine. */

    /* Wait for remote machine to complete TLB flush and send reply */
    /* The reply will be placed in our slot (msi_test_msg[my_id]) */
    /* We poll for the reply even in MSI mode to avoid waiting for interrupts in
     * page fault context */
    u32 max_wait_iters = 1000000; /* Prevent infinite wait */
    u32 iter = 0;
    while (iter < max_wait_iters) {
        /* Poll for messages sent to us (to check for reply from remote machine)
         */
        /* The remote machine will process our message via MSI interrupt handler
         */

        /* Check if we received a reply */
        lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        u32 reply_received = dsm_meta->msi_test_msg[my_id].reply_received;
        u32 reply_from = dsm_meta->msi_test_msg[my_id].reply_from;
        unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);

        if (reply_received == 1 && reply_from == machine_id) {
            /* Remote machine has completed TLB flush */
            break;
        }
        /* Small delay to avoid busy waiting */
        asm volatile("pause");
        iter++;
    }

    /* Clear the reply flag for next use */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 0;
    dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);

    if (iter >= max_wait_iters) {
        kwarn("[TLB] Timeout waiting for TLB flush reply from machine %d\n",
              machine_id);
        return -ETIMEDOUT;
    }
    return 0;
}

/*
 * Drop every translation this vmspace may still have on any other machine.
 *
 * flush_tlbs() only reaches the CPUs of the machine that calls it, because
 * vmspace->history_cpus[] is PLAT_CPU_NUM bytes indexed by the machine-local
 * CPU id and every machine aliases the same slots.  This is the cluster-wide
 * counterpart, driven by history_machines[].
 *
 * Range-less on purpose: the remote handler (ivshmem_handle_tlb_flush_msg)
 * flushes the whole TLB regardless of the address it is given, so one message
 * per machine covers the entire vmspace.
 *
 * Note the request/reply protocol has a single slot per machine, so two
 * senders targeting the same machine can clobber each other; the loser sees no
 * reply and retries here rather than silently skipping the flush.
 */
void flush_tlbs_all_machines(struct vmspace *vmspace)
{
    const int max_attempts = 3;
    mid_t my_id = CUR_MACHINE_ID;
    int i, attempt;

    if (!dsm_meta)
        return;

    for (i = 0; i < CLUSTER_MACHINE_NUM && i < CLUSTER_MAX_MACHINE_NUM; i++) {
        if (i == my_id || !vmspace->history_machines[i])
            continue;

        for (attempt = 0; attempt < max_attempts; attempt++) {
            if (flush_tlb_on_remote_machine(vmspace, i, 0, PAGE_SIZE) == 0)
                break;
        }
        if (attempt == max_attempts) {
            kwarn("[TLB] machine %d did not acknowledge the vmspace %p "
                  "shootdown; it may still hold stale translations\n",
                  i, vmspace);
        }
    }
}

/* Internal implementation for MSI mode */
static void memcpy_and_flush_tlb_on_remote_machine_msi(
        struct vmspace *vmspace, mid_t target_mid, paddr_t src_pa,
        paddr_t dst_pa, size_t len, vaddr_t fault_va)
{
    mid_t my_id = CUR_MACHINE_ID;

    if (!dsm_meta || target_mid >= CLUSTER_MACHINE_NUM || target_mid == my_id)
        return;

    /* Prepare message in target machine's slot (with lock protection) */
    lock(&dsm_meta->msi_test_msg[target_mid].msg_lock);
    dsm_meta->msi_test_msg[target_mid].msg_from = my_id;
    dsm_meta->msi_test_msg[target_mid].msg_type =
            MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB;
    dsm_meta->msi_test_msg[target_mid].reply_received = 0;
    dsm_meta->msi_test_msg[target_mid].reply_from = 0xFFFFFFFF;
    dsm_meta->msi_test_msg[target_mid].memcpy_src_pa = src_pa;
    dsm_meta->msi_test_msg[target_mid].memcpy_dst_pa = dst_pa;
    dsm_meta->msi_test_msg[target_mid].memcpy_len = len;
    dsm_meta->msi_test_msg[target_mid].memcpy_fault_va = fault_va;
    dsm_meta->msi_test_msg[target_mid].memcpy_vmspace = (u64)vmspace;
    unlock(&dsm_meta->msi_test_msg[target_mid].msg_lock);

    kinfo("[TLB] Machine %d sending memcpy+flush_tlb request to machine %d (src_pa=0x%llx, dst_pa=0x%llx, len=%zu)\n",
          my_id,
          target_mid,
          src_pa,
          dst_pa,
          len);

    /* Clear our own slot's reply flag before sending request */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 0;
    dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);

    /* Send MSI to notify remote machine */
    extern int ivshmem_send_msi(mid_t target_machine_id, u16 vector);
    int ret = ivshmem_send_msi(target_mid, 0); /* Use vector 0 for memcpy+TLB
                                                  flush */
    kinfo("[TLB] Sent MSI to machine %d, ret=%d\n", target_mid, ret);
    /* Note: We still poll for reply even in MSI mode because we're in a page
     * fault handler and cannot wait for interrupts. The MSI will trigger
     * processing on the remote machine. */

    /* Poll for remote machine to complete memcpy and flush TLB, then send reply
     */
    /* The reply will be placed in our slot (msi_test_msg[my_id]) */
    /* We poll for the reply even in MSI mode to avoid waiting for interrupts in
     * page fault context */
    while (true) {
        /* Check if we received a reply */
        lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
        u32 reply_received = dsm_meta->msi_test_msg[my_id].reply_received;
        u32 reply_from = dsm_meta->msi_test_msg[my_id].reply_from;
        unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);

        if (reply_received == 1 && reply_from == target_mid) {
            /* Remote machine has completed memcpy and flush TLB */
            kinfo("[TLB] Machine %d received reply from machine %d\n",
                  my_id,
                  target_mid);
            break;
        }
        /* Small delay to avoid busy waiting */
        asm volatile("pause");
    }

    /* Clear reply flags after receiving reply or timing out */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 0;
    dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
}

/*
 * Batched variant of the polling path: one request carries a run of pages.
 *
 * Returns the remote machine's result, or -EIO when the request could not be
 * delivered at all.  sys_memcpy_and_flush_tlb_batch() is all-or-nothing, so a
 * non-zero result means no entry in the batch was applied.
 */
static int memcpy_and_flush_tlb_batch_on_remote_machine_polling(
        struct vmspace *vmspace, mid_t target_mid,
        struct polling_tlb_batch_entry *entries, u64 count)
{
    mid_t my_id = machine_id;
    struct polling_request req;
    struct dq_node *msg;
    u64 i;
    int ret;

    if (target_mid >= CLUSTER_MACHINE_NUM || target_mid == my_id) {
        kwarn("[TLB] Invalid batch migration target: %d\n", target_mid);
        return -EIO;
    }

    struct polling_shm_region *target_shm =
            (struct polling_shm_region *)dsm_meta->shm_data[target_mid].data;

    if (!target_shm) {
        kwarn("[TLB] Polling shm region is NULL: target_shm=%p\n", target_shm);
        return -EIO;
    }

    req.type = POLLING_KERNEL_REQ_FLUSH_TLB_BATCH;
    req.flush_tlb_batch.memcpy_vmspace = (u64)vmspace;
    req.flush_tlb_batch.memcpy_len = PAGE_SIZE;
    req.flush_tlb_batch.count = count;
    for (i = 0; i < count; i++)
        req.flush_tlb_batch.entries[i] = entries[i];

    msg = dq_alloc_node(target_shm);
    dq_enqueue(target_shm, msg, &req);
    dq_wait_for_done(msg);
    /*
     * Read the reply before releasing the node: once it is CONSUMED the server
     * may recycle it and overwrite the response.
     */
    ret = msg->resp.flush_tlb.reply_result;
    /*
     * The server defers recycling a node until its producer marks it CONSUMED.
     * Skipping this leaves the node at DQ_DONE forever, and once it reaches the
     * eviction slot of the server's deferred-free ring that machine stops
     * dequeuing anything at all.
     */
    dq_mark_consumed(msg);

    if (ret != 0)
        kwarn("[TLB] Batch migration of %lu pages to machine %d failed: %d\n",
              count, target_mid, ret);
    return ret;
}

/*
 * Migrate a run of pages to shared memory in one round trip.
 *
 * Entries must already carry src_pa/dst_pa/fault_va; the caller owns the
 * destination pages and the migrating-VA reservations.  The remote machine
 * pays two all-CPU TLB shootdowns for the whole batch instead of two per page.
 *
 * Returns 0 when every entry was migrated.  On a non-zero return nothing was
 * migrated and the caller must not map any dst_pa: those pages were never
 * written, so mapping them would hand the process uninitialized memory.
 */
int migrate_pages_to_shm_batch(mid_t target_mid, struct vmspace *vmspace,
                               struct polling_tlb_batch_entry *entries,
                               u64 count)
{
    extern enum ivshmem_msg_mode ivshmem_get_msg_mode(void);
    u64 i;

    if (count == 0)
        return 0;

    if (ivshmem_get_msg_mode() == IVSHMEM_MSG_MODE_MSI) {
        /*
         * MSI transport has no batch message; fall back to one page at a time.
         * That path carries no result either, so it keeps the historical
         * behaviour of assuming success.
         */
        for (i = 0; i < count; i++)
            memcpy_and_flush_tlb_on_remote_machine_msi(
                    vmspace, target_mid, entries[i].src_pa, entries[i].dst_pa,
                    PAGE_SIZE, entries[i].fault_va);
        return 0;
    }

    return memcpy_and_flush_tlb_batch_on_remote_machine_polling(
            vmspace, target_mid, entries, count);
}

#endif
