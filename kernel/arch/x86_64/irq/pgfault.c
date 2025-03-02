#include <common/types.h>
#include <object/thread.h>
#include <mm/vmspace.h>
#include <arch/mm/page_table.h>

// int handle_trans_fault(struct vmspace *vmspace, vaddr_t fault_addr);

int handle_trans_fault(struct vmspace *vmspace, vaddr_t fault_addr, int present,
                       int write);

static inline vaddr_t get_fault_addr()
{
    vaddr_t addr;

    asm volatile("mov %%cr2, %0" : "=r"(addr)::);
    return addr;
}

#define FLAG_P  (1 << 0)
#define FLAG_RW (1 << 1)
#define FLAG_US (1 << 2)
#define FLAG_ID (1 << 4)

/*
 * Note: when we considering COW that needs to flush TLB
 * after triggerring page fault (modify the permission in the page table),
 * pay attention to flush TLB without holding lock.
 *
 * For example:
 * If CPU0 acquires the vmspace_lock and sends IPI (flushtlb)
 * to CPU1, but CPU1 is now in a #PF IRQ context and also
 * trys to acquire the vmspace_lock,
 * dead lock happens.
 * Specifically, CPU0 holds the lock and waits for CPU1
 * while CPU1 cannot make progress due to failing to lock.
 *
 */

extern int query_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t *pa, pte_t **entry);
/* Handle Page Fault Here */
void do_page_fault(u64 errorcode, u64 fault_ins_addr)
{
    vaddr_t fault_addr;
    int ret;
    paddr_t pte_pa;
    pte_t *pte;

    /*
     * errorcode
     *
     * - P flag (bit 0). This flag is 0 if there is no translation for the
     *   linear address because the P flag was 0 in one of the paging-
     *   structure entries used to translate that address.
     *
     * - W/R (bit 1). If the access causing the page-fault exception was a
     *   write, this flag is 1; otherwise, it is 0. This flag
     *   describes the access causing the page-fault exception, not the
     *   access rights specified by paging.
     *
     * - U/S (bit 2). If a user-mode access caused the page-fault exception,
     *   this flag is 1; it is 0 if a supervisor-mode access did
     *   so.
     *
     * - I/D (bit 4). 0: The fault was not caused by an instruction fetch.
     *   1: The fault was caused by an instruction fetch.
     *
     * - PKEY (bit 5). 0: The fault was not caused by protection keys.
     *   1: There was a protection-key violation.
     *
     * - SGX (bit 15). 0 The fault is not related SGX.
     *   1 The fault resulted from violation of SGX-specific
     *   access-control requirements.
     *
     */

    fault_addr = get_fault_addr();

    // TODO(FN): handle this case in handle_trans_fault
#if 0
	if (errorcode & FLAG_P) {
		kinfo("[pgfault handler] permission error.\n");
		if (errorcode & FLAG_RW)
			kinfo("Cannot write at %p.\n", fault_addr);
		else
			kinfo("Cannot read at %p.\n", fault_addr);
		ret = -1;
	} else {
#endif
    if (current_thread == NULL) {
        kinfo("Fault_addr %p fault_ins_addr %p\n", fault_addr, fault_ins_addr);
        BUG_ON(1);
    }
    ret = handle_trans_fault(current_thread->vmspace,
                             fault_addr,
                             errorcode & FLAG_P,
                             errorcode & FLAG_RW);
    kdebug("%s: current_thread->vmspace is %p, fault_addr is 0x%lx\n",
           __func__,
           current_thread->vmspace,
           fault_addr);
#if 0
	}
#endif
    if (ret != 0) {
        if (errorcode & FLAG_ID)
            kinfo("Fault caused by instruction fetch\n");
        else
            kinfo("Fault caused by data access\n");

        if (errorcode & FLAG_US)
            kinfo("Fault caused by User-mode access\n");
        else
            kinfo("Fault caused by Kernel-mode access\n");

        kinfo("do_page_fault: faulting ip is 0x%lx,"
              "faulting address is 0x%lx,"
              "errorcode is (0b%b)\n",
              fault_ins_addr,
              fault_addr,
              errorcode);

        query_in_pgtbl(
                current_thread->vmspace->pgtbl, fault_addr, &pte_pa, &pte);
        if (pte) {
            kinfo("Fault pte value: 0x%lx\n", *pte);
        }

        kprint_vmr(current_thread->vmspace);

        kinfo("current_cap_group is %s\n", current_cap_group->cap_group_name);

        // TODO: kill the process
        BUG_ON(1);
    }
}
