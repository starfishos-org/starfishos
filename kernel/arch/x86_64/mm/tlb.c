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

    if (unlikely(len < PAGE_SIZE))
        kwarn("func: %s. len (%p) < PAGE_SIZE\n", __func__, len);

    if (len == 0)
        return;

    len = ROUND_UP(len, PAGE_SIZE);
    page_cnt = len / PAGE_SIZE;

    pcid = get_pcid(vmspace->pgtbl);

    /* Flush local TLBs */
    flush_local_tlb_opt(start_va, page_cnt, pcid);

    cpuid = smp_get_cpu_id();
    /* Flush remote TLBs */
    // TODO: it may be, sometimes, effective to interrupt all other CPU at the
    // same time.
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if ((i != cpuid) && (vmspace->history_cpus[i] == 1)) {
            flush_remote_tlb_with_ipi(
                    i, start_va, page_cnt, pcid, (u64)vmspace);
        }
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

    /* Flush local TLBs */
    flush_local_tlb_opt(dummy_va, page_cnt, pcid);

    cpuid = smp_get_cpu_id();
    /* Flush remote TLBs */
    for (i = 0; i < PLAT_CPU_NUM; ++i) {
        if ((i != cpuid) && (vmspace->history_cpus[i] == 1)) {
            flush_remote_tlb_with_ipi(
                    i, dummy_va, page_cnt, pcid, dummy_vmspace);
        }
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

#if 0
    mid_t my_id = CUR_MACHINE_ID;
    
    if (target_mid >= CLUSTER_MACHINE_NUM || target_mid == my_id)
        return;
    
    /* Each machine has its own shared memory region: dsm_meta->shm_data[machine_id] */
    struct polling_shm_region *target_shm = (struct polling_shm_region *)dsm_meta->shm_data[target_mid].data;
    struct polling_shm_region *my_shm = (struct polling_shm_region *)dsm_meta->shm_data[my_id].data;
    
    if (!target_shm || !my_shm) {
        kwarn("[TLB] Polling shm region is NULL: target_shm=%p, my_shm=%p\n", target_shm, my_shm);
        return;
    }
    
    /* Find a free message slot in target machine's region */
    int target_slot = -1;
    for (int i = 0; i < MAX_MSG_COUNT; i++) {
        u32 magic = target_shm->msgs[i].magic;
        enum shm_msg_flag flag = target_shm->msgs[i].flag;
        u32 expected_magic = SHM_MSG_MAGIC(i);
        if (magic == expected_magic && flag == SHM_MSG_FREE) {
            /* Try to acquire lock */
            if (compare_and_swap_32((s32 *)&target_shm->msgs[i].lock.slock, 0, 1) == 0) {
                target_slot = i;
                break;
            }
        } else if (magic != expected_magic && magic != SHM_MSG_MAGIC_INVALID) {
            /* Log magic mismatch for debugging */
            kinfo("[TLB] Machine %d: Target machine %d slot %d magic mismatch! Expected 0x%x, got 0x%x\n",
                  my_id, target_mid, i, expected_magic, magic);
        }
    }
    
    if (target_slot < 0) {
        kwarn("[TLB] Machine %d: No free message slot found in machine %d's region\n", my_id, target_mid);
        return;
    }
    
    struct shm_msg *target_msg = &target_shm->msgs[target_slot];
    
    /* Verify magic number before writing */
    u32 expected_magic = SHM_MSG_MAGIC(target_slot);
    if (target_msg->magic != expected_magic) {
        kwarn("[TLB] Machine %d: Target machine %d slot %d magic mismatch before write! Expected 0x%x, got 0x%x\n",
              my_id, target_mid, target_slot, expected_magic, target_msg->magic);
        target_msg->lock.slock = 0; /* Release lock */
        return;
    }
    
    /* Prepare message in target machine's slot */
    kinfo("[TLB] Machine %d preparing message to machine %d: target_slot=%d, magic=0x%x\n",
          my_id, target_mid, target_slot, expected_magic);
    
    /* Write request to target machine's slot - reply will be in the same slot */
    /* Note: We don't modify magic, it should remain SHM_MSG_MAGIC(target_slot) */
    target_msg->type = SHM_MSG_TYPE_TLB_REQ;  /* Same as MSI_MSG_TYPE_MEMCPY_AND_FLUSH_TLB (value 2) */
    target_msg->sender = my_id;
    target_msg->msg.tlb_req.memcpy_src_pa = src_pa;
    target_msg->msg.tlb_req.memcpy_dst_pa = dst_pa;
    target_msg->msg.tlb_req.memcpy_len = len;
    target_msg->msg.tlb_req.memcpy_fault_va = fault_va;
    target_msg->msg.tlb_req.memcpy_vmspace = (u64)vmspace;
    /* Initialize reply fields in the same slot */
    target_msg->msg.tlb_req.reply_received = 0;
    target_msg->msg.tlb_req.reply_from = 0xFFFFFFFF;
    target_msg->msg.tlb_req.reply_result = 0;

    /* Use memory barrier to ensure all writes are visible before setting flag */
    wmb();
    /* Set flag to indicate message is readable */
    target_msg->flag = SHM_MSG_READABLE;
    
    /* Release lock */
    target_msg->lock.slock = 0;
    
    kinfo("[TLB] Machine %d sent memcpy+flush_tlb request to machine %d (polling mode): "
          "target_slot=%d, sender=%u, type=%u, src_pa=0x%llx, dst_pa=0x%llx, len=%zu, flag=%u\n",
          my_id, target_mid, target_slot,
          target_msg->sender, target_msg->type,
          target_msg->msg.tlb_req.memcpy_src_pa, target_msg->msg.tlb_req.memcpy_dst_pa, 
          target_msg->msg.tlb_req.memcpy_len, target_msg->flag);
    
    /* Wait for remote machine to process the message and send reply */
    /* In polling mode, the remote machine's user-space polling server will process
     * the message we just sent. The reply will be written back to the same slot (target_msg). */
    u64 wait_count = 0;
    while (true) {
        /* Check if we received a reply in the same slot where we sent the request */
        /* Use read memory barrier to ensure we see all writes before reply_received was set */
        rmb(); /* Acquire semantics: ensure we see all writes before reply_received */
        u32 reply_received = target_msg->msg.tlb_req.reply_received;
        u32 reply_from = target_msg->msg.tlb_req.reply_from;
        
        if (wait_count % 1000000 == 0 && wait_count > 0) {
            kinfo("[TLB] Machine %d waiting for reply from machine %d (slot %d): reply_received=%u, reply_from=%u (waited %llu times)\n",
                  my_id, target_mid, target_slot, reply_received, reply_from, wait_count);
        }
        
        if (reply_received == 1) {
            /* After seeing reply_received == 1 with memory barrier, 
             * we can safely read reply_from and reply_result */
            s32 reply_result = target_msg->msg.tlb_req.reply_result;
            if (reply_from == target_mid) {
                kinfo("[TLB] Machine %d received reply from machine %d (polling mode, slot %d, result=%d, waited %llu times)\n",
                      my_id, target_mid, target_slot, reply_result, wait_count);
                break;
            } else {
                /* Log unexpected reply_from */
                if (wait_count % 1000000 == 0) {
                    kinfo("[TLB] Machine %d waiting for reply from machine %d but got reply_from=%u (slot %d, waited %llu times)\n",
                          my_id, target_mid, reply_from, target_slot, wait_count);
                }
            }
        }
        wait_count++;
        /* Small delay to avoid busy waiting */
        asm volatile("pause");
    }

    /* Clear target message slot after receiving reply */
    /* Verify magic is still correct before clearing */
    if (target_msg->magic != expected_magic) {
        kwarn("[TLB] Machine %d: Target machine %d slot %d magic changed after reply! Expected 0x%x, got 0x%x\n",
              my_id, target_mid, target_slot, expected_magic, target_msg->magic);
    }
    target_msg->flag = SHM_MSG_FREE;
    target_msg->type = SHM_MSG_TYPE_MAX;
    target_msg->sender = 0xFFFFFFFF;
    target_msg->msg.tlb_req.reply_received = 0;
    target_msg->msg.tlb_req.reply_from = 0xFFFFFFFF;
    target_msg->msg.tlb_req.reply_result = 0;
    /* Note: We keep magic unchanged, it should remain SHM_MSG_MAGIC(target_slot) */
#endif
}

/* Unified interface: automatically selects MSI or polling mode based on current
 * configuration */
void memcpy_and_flush_tlb_on_remote_machine(struct vmspace *vmspace,
                                            mid_t target_mid, paddr_t src_pa,
                                            paddr_t dst_pa, size_t len,
                                            vaddr_t fault_va)
{
    extern enum ivshmem_msg_mode ivshmem_get_msg_mode(void);
    enum ivshmem_msg_mode mode = ivshmem_get_msg_mode();

    if (mode == IVSHMEM_MSG_MODE_MSI) {
        memcpy_and_flush_tlb_on_remote_machine_msi(
                vmspace, target_mid, src_pa, dst_pa, len, fault_va);
    } else {
        memcpy_and_flush_tlb_on_remote_machine_polling(
                vmspace, target_mid, src_pa, dst_pa, len, fault_va);
    }
}
#endif
