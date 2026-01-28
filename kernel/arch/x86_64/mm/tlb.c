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
    flush_local_tlb_opt(start_va, page_cnt, pcid);

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

void flush_tlbs(struct vmspace *vmspace, vaddr_t start_va, size_t len)
{
    flush_tlb_local_and_remote(vmspace, start_va, len);
}

#ifdef MULTI_PAGETABLE_ENABLED
#include <common/lock.h>

/* Flush TLB only for CPUs belonging to a specific machine */
void flush_tlb_on_remote_machine(struct vmspace *vmspace, mid_t machine_id,
                                 vaddr_t start_va, size_t len)
{
    mid_t my_id = CUR_MACHINE_ID;

    if (!dsm_meta || machine_id >= CLUSTER_MACHINE_NUM || machine_id == my_id)
        return;

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

    if (iter >= max_wait_iters) {
        kwarn("[TLB] Timeout waiting for TLB flush reply from machine %d\n",
              machine_id);
    }

    /* Clear the reply flag for next use */
    lock(&dsm_meta->msi_test_msg[my_id].msg_lock);
    dsm_meta->msi_test_msg[my_id].reply_received = 0;
    dsm_meta->msi_test_msg[my_id].reply_from = 0xFFFFFFFF;
    unlock(&dsm_meta->msi_test_msg[my_id].msg_lock);
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

/* Internal implementation for polling mode */
static void memcpy_and_flush_tlb_on_remote_machine_polling(
        struct vmspace *vmspace, mid_t target_mid, paddr_t src_pa,
        paddr_t dst_pa, size_t len, vaddr_t fault_va)
{
    mid_t my_id = machine_id;

    if (target_mid >= CLUSTER_MACHINE_NUM || target_mid == my_id)
        return;

    struct polling_shm_region *target_shm =
            (struct polling_shm_region *)dsm_meta->shm_data[target_mid].data;

    if (!target_shm) {
        kwarn("[TLB] Polling shm region is NULL: target_shm=%p\n", target_shm);
        return;
    }

    struct polling_request req = {
            .type = POLLING_KERNEL_REQ_FLUSH_TLB,
            .flush_tlb =
                    {
                            .memcpy_src_pa = src_pa,
                            .memcpy_dst_pa = dst_pa,
                            .memcpy_len = len,
                            .memcpy_fault_va = fault_va,
                            .memcpy_vmspace = (u64)vmspace,
                    },
    };

    struct shm_msg *msg = mpsc_alloc_msg(target_shm);

    polling_publish_request(msg, &req);

    polling_wait_for_response(msg);

    polling_free_msg(msg);
}

/* Migrate pages to shared memory: wrapper function with simplified interface */
paddr_t migrate_pages_to_shm(mid_t target_mid, struct vmspace *vmspace,
                              paddr_t src_pa, size_t len, vaddr_t fault_va)
{
    void *new_va;
    paddr_t dst_pa;

    /* Allocate shared memory page on current machine */
    new_va = get_pages(0, __MT_SHARED__);
    BUG_ON(new_va == NULL);
    dst_pa = virt_to_phys(new_va);

    /* Call the underlying function to perform memcpy and flush TLB */
    extern enum ivshmem_msg_mode ivshmem_get_msg_mode(void);
    enum ivshmem_msg_mode mode = ivshmem_get_msg_mode();

    if (mode == IVSHMEM_MSG_MODE_MSI) {
        memcpy_and_flush_tlb_on_remote_machine_msi(
                vmspace, target_mid, src_pa, dst_pa, len, fault_va);
    } else {
        memcpy_and_flush_tlb_on_remote_machine_polling(
                vmspace, target_mid, src_pa, dst_pa, len, fault_va);
    }

    return dst_pa;
}
#endif
