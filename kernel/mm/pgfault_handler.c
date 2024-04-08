#include <common/macro.h>
#include <common/util.h>
#include <common/list.h>
#include <common/errno.h>
#include <common/kprint.h>
#include <common/types.h>
#include <common/lock.h>
#include <lib/printk.h>
#include <mm/vmspace.h>
#include <mm/kmalloc.h>
#include <mm/buddy.h>
#include <mm/mm.h>
#include <mm/nvm.h>
#include <mm/vmspace.h>
#include <mm/nvm.h>
#include <mm/rmap.h>
#include <arch/mmu.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include <object/user_fault.h>
#include <sched/context.h>
#include <arch/sync.h>
#include <mm/page.h>
#include <arch/mm/page_table.h>
#include <object/recycle.h>
#ifdef CHCORE_SLS
#include <ckpt/hot_pages_tracker.h>
#include <ckpt/ckpt.h>
#endif

/* Policy on-demand: only mapping the faulting address */
#define ONDEMAND 0

/*
 * Not implemented now.
 * Policy pre-fault: mapping the serveral continous pages in advance.
 */
#define PREFAULT 1

#define PGFAULT_POLICY ONDEMAND

#if PGFAULT_POLICY == ONDEMAND

#ifdef CHCORE_SLS
/* add_pte_patch_to_pool: when trigger write, track pages's pte and page struct
 */
void add_pte_patch_to_pool(struct vmspace *vmspace, pte_t *pte,
                           struct page *page)
{
        struct pte_patch_pool *pool, *new_pool;

        pool = (struct pte_patch_pool *)vmspace->pte_patch_pool;
        if (pool) {
                /* set next pte entry */
                /* check if vmspace lock is already get? */
                // lock(&vmspace->pte_patch_lock);
                pool->array[pool->count] =
                        (struct pte_patch_pool_entry){.pte = pte, .page = page};
                // printk("add_pte_patch_to_pool: page=%p\n", page);
                if (++pool->count >= MAX_ENTRY) {
                        /* Full, alloc a new pool */
                        new_pool = (struct pte_patch_pool *)create_patch_pool();
                        new_pool->next = pool;
                        vmspace->pte_patch_pool = new_pool;
                }
                // unlock(&vmspace->pte_patch_lock);
        } else {
                // kinfo("[ERR] empty pte_patch_pool\n");
        }
}

#ifdef OMIT_BENCHMARK
static inline bool need_omit(struct vmspace *vmspace)
{
        return vmspace == redis_benchmark_vmspace
               || vmspace == memcachetest_vmspace || vmspace == ycsb_vmspace;
}
#endif
#ifdef REPORT
u64 patch_page_num = 0;
#endif
#ifdef REPORT_RUNTIME
extern u64 pf_count;
extern u64 pf_tot_time;
#endif
#endif /* CHCORE_SLS */

int map_page_in_pgtbl(void *pgtbl, vaddr_t va, paddr_t pa, vmr_prop_t flags,
                      pte_t **out_pte);
int handle_trans_fault(struct vmspace *vmspace, vaddr_t fault_addr, int present,
                       int write)
{
#ifdef REPORT_RUNTIME
        DECLTMR;
        start();
#endif
        struct vmregion *vmr;
        struct pmobject *pmo;
        struct page *page;
        paddr_t pa;
        u64 offset;
        u64 index;
        int ret = 0;
        pte_t *pte; /* out pte */
#ifdef CHCORE_SLS
        int ckpt_ret = 0;
#if defined(OMIT_PF) && !defined(PMO_CHECKSUM)
        UNUSED(page);
#endif
#endif
        /*
         * Grab lock here.
         * Because two threads (in same process) on different cores
         * may fault on the same page, so we need to prevent them
         * from adding the same mapping twice.
         */
        read_lock(&vmspace->vmspace_lock);
        vmr = find_vmr_for_va(vmspace, fault_addr);
        if (vmr == NULL) {
                kinfo("handle_trans_fault: no vmr found for va 0x%lx!\n",
                      fault_addr);
                // TODO: kill the process
                kwarn("TODO: kill such faulting process.\n");
                sys_exit_group(-1);
                return -ENOMAPPING;
        }

        pmo = vmr->pmo;
        switch (pmo->type) {
        case PMO_DATA:
        case PMO_FILE:
#ifdef CHCORE_SLS
        case PMO_RING_BUFFER:
        case PMO_RING_BUFFER_RADIX:
#endif
        case PMO_ANONYM:
#ifdef USE_CXL_MEM
        case PMO_CROSS_SHM:
#endif
        case PMO_SHM: {
                vmr_prop_t perm;

                perm = vmr->perm;

                /* Get the offset in the pmo for faulting addr */
                offset = ROUND_DOWN(fault_addr, PAGE_SIZE) - vmr->start;
                /* Boundary check */
                if ((offset >= pmo->size) && (pmo->type == PMO_FILE)) {
                        static int warn = 1;

                        if (warn == 1) {
                                kwarn("%s (out-of-range writing) offset 0x%lx, "
                                      "pmo->size 0x%lx, FILE\n",
                                      __func__,
                                      offset,
                                      pmo->size);
                                warn = 0;
                        }

                        /*
                         * FIXME: we simply allow it now by adding new pages.
                         * For PMO_FILE, users can mmap a memory that is larger
                         * than the file size. If they accesses bytes beyond
                         * file size, SIGBUS should be triggered on LInux.
                         *
                         * TODO: why setting all the perm here?
                         */
                        perm = VMR_READ | VMR_WRITE | VMR_EXEC;
                } else {
                        BUG_ON(offset >= pmo->size);
                }

                /* Get the index in the pmo radix for faulting addr */
                index = offset / PAGE_SIZE;

                fault_addr = ROUND_DOWN(fault_addr, PAGE_SIZE);

                pa = get_page_from_pmo(pmo, index);

                /* PMO_FILE fault means user fault */
                if (pmo->type == PMO_FILE && !pa) {
                        /* pa != 0 means this fault is cause by ckpt/restore */
#ifdef CHCORE_ENABLE_FMAP
                        read_unlock(&vmspace->vmspace_lock);
                        handle_user_fault(pmo, fault_addr);
                        /* One short-cut exit */
                        BUG("Should never be here!\n");
#else
                        ret = -EINVAL;
                        break;
#endif
                }

                if (pa == 0) {
                        /* Not committed before. Then, allocate the physical
                         * page. */
                        void *new_va;
#if 0
                        if (pmo->type == PMO_CROSS_SHM)
                                new_va = get_cxl_pages(0);
                        else
#endif
                        new_va = get_pages(0, __DEFAULT__);
                        BUG_ON(new_va == NULL);
                        pa = virt_to_phys(new_va);
                        BUG_ON(pa == 0);
                        /* Clear to 0 for the newly allocated page */
                        memset(new_va, 0, PAGE_SIZE);
                        /*
                         * Record the physical page in the radix tree:
                         * the offset is used as index in the radix tree
                         */
                        kdebug("commit: index: %ld, 0x%lx\n", index, pa);
                        commit_page_to_pmo(pmo, index, pa);

                        /* Add mapping in the page table */
                        lock(&vmspace->pgtbl_lock);
                        map_page_in_pgtbl(
                                vmspace->pgtbl, fault_addr, pa, perm, &pte);
                        unlock(&vmspace->pgtbl_lock);

#ifdef CHCORE_SLS
                        /* do not persist pages belong to external sync pmo */
                        if (is_external_sync_pmo(pmo))
                                break;
#ifdef OMIT_BENCHMARK
                        /* omit track page fault of redis-benchmark */
                        if (need_omit(vmspace)) {
                                break;
                        }
#endif
#ifndef OMIT_PF
                        page = virt_to_page(new_va);
                        if ((vmspace->flags & VM_FLAG_PRESERVE)
                            && !is_external_sync_pmo(pmo)) {
#ifdef PMO_CHECKSUM
                                page->ckpt_version_number =
                                        get_current_ckpt_version() + 1;
#endif
#ifndef OMIT_BENCHMARK
                                ckpt_nvm_page(pmo, new_va, index);
#endif
                                add_pte_patch_to_pool(
                                        vmspace, (pte_t *)pte, page);
                        }
#endif
#endif /* CHCORE_SLS */
                } else {
                        /*                                                     \
                         * pa != 0: the faulting address has be committed a    \
                         * physical page.                                      \
                         *                                                     \
                         * For concurrent page faults:                         \
                         *                                                     \
                         * When type is PMO_ANONYM, the later faulting threads \
                         * of the process do not need to modify the page       \
                         * table because a previous faulting thread will do    \
                         * that. (This is always true for the same process)    \
                         * However, if one process map an anonymous pmo for    \
                         * another process (e.g., main stack pmo), the faulting
                         * thread (e.g, in the new process) needs to update    \
                         * its page table. \
                         * So, for simplicity, we just update the page table.  \
                         * Note that adding the same mapping is harmless.      \
                         *                                                     \
                         * When type is PMO_SHM, the later faulting threads    \
                         * needs to add the mapping in the page table.         \
                         * Repeated mapping operations are harmless.           \
                         */
                        /* For PMO_FILE, we simply set all the perm now. */
                        if (pmo->type == PMO_FILE) {
                                perm = VMR_READ | VMR_WRITE | VMR_EXEC;
                        }
#ifdef CHCORE_SLS
#ifndef OMIT_PF
                        if ((vmspace->flags & VM_FLAG_PRESERVE) && !write
                            && !is_external_sync_pmo(pmo)) {
                                /* Read preserved page, map as read-only */
                                perm &= ~VMR_WRITE;
                        }
#endif
#endif /* CHCORE_SLS */
                        /* handle COW */
                        if (!is_shared_pmo(pmo)) {
                                if (use_continuous_pages(pmo)) {
                                        page = virt_to_page(
                                                (void *)phys_to_virt(
                                                        pmo->start));
                                        lock(&page->lock);
                                        if (page->ref_cnt > 1) {
                                                void *new_va = kmalloc(
                                                        pmo->size, __DEFAULT__);
                                                if (new_va == NULL) {
                                                        ret = -ENOMEM;
                                                        unlock(&page->lock);
                                                        break;
                                                }

                                                memcpy(new_va,
                                                       (void *)phys_to_virt(
                                                               pmo->start),
                                                       pmo->size);
                                                pmo->start =
                                                        virt_to_phys(new_va);
                                                /* new pa */
                                                pa = pmo->start
                                                     + index * PAGE_SIZE;

                                                lock(&vmspace->pgtbl_lock);
                                                if ((vmspace->flags
                                                     & VM_FLAG_PRESERVE)) {
                                                        map_range_in_pgtbl(
                                                                vmspace->pgtbl,
                                                                vmr->start,
                                                                pmo->start,
                                                                pmo->size,
                                                                perm & (~VMR_WRITE));
                                                } else {
                                                        map_range_in_pgtbl(
                                                                vmspace->pgtbl,
                                                                vmr->start,
                                                                pmo->start,
                                                                pmo->size,
                                                                perm);
                                                }
                                                unlock(&vmspace->pgtbl_lock);

                                                flush_tlbs(vmspace,
                                                           vmr->start,
                                                           vmr->size);
                                                atomic_fetch_sub_64(
                                                        &page->ref_cnt, 1);
                                        }
                                        unlock(&page->lock);
                                } else {
                                        int cow = false;
                                        page = virt_to_page(
                                                (void *)phys_to_virt(pa));
                                        lock(&page->lock);
                                        if (page->ref_cnt > 1) {
                                                void *new_va = get_pages(
                                                        0, __DEFAULT__);
                                                if (new_va == NULL) {
                                                        ret = -ENOMEM;
                                                        unlock(&page->lock);
                                                        break;
                                                }

                                                pagecpy_nt(new_va,
                                                           (void *)phys_to_virt(
                                                                   pa));
                                                /* new pa */
                                                pa = virt_to_phys(new_va);

                                                lock(&vmspace->pgtbl_lock);
                                                map_page_in_pgtbl(
                                                        vmspace->pgtbl,
                                                        fault_addr,
                                                        pa,
                                                        perm,
                                                        &pte);
                                                unlock(&vmspace->pgtbl_lock);

                                                flush_tlbs(vmspace,
                                                           fault_addr,
                                                           PAGE_SIZE);
                                                atomic_fetch_sub_64(
                                                        &page->ref_cnt, 1);
                                                cow = true;
                                        }
                                        unlock(&page->lock);
                                        if (cow) {
                                                commit_page_to_pmo(
                                                        pmo, index, pa);
                                                goto skip_add_mapping;
                                        }
                                }
                        }

                        /* Add mapping in the page table */
                        pte_t *pte = NULL;
                        lock(&vmspace->pgtbl_lock);
                        map_page_in_pgtbl(
                                vmspace->pgtbl, fault_addr, pa, perm, &pte);
                        unlock(&vmspace->pgtbl_lock);
                skip_add_mapping:
                        kdebug("[CoW] skip add mapping\n");
#ifdef CHCORE_SLS

                        /* do not persist pages belong to external sync pmo */
                        if (is_external_sync_pmo(pmo))
                                break;
#ifndef OMIT_PF
#ifdef OMIT_BENCHMARK
                        /* omit track page fault of benchmarks */
                        if (need_omit(vmspace)) {
                                break;
                        }
#endif

                        if (write && (vmspace->flags & VM_FLAG_PRESERVE)) {
                                page = virt_to_page((void *)phys_to_virt(pa));
                                BUG_ON(unlikely(!page));
                                if (unlikely(get_page_type(page) != NVM_PAGE)) {
                                        /* Dram page will be marked as
                                         * unwritable after fork */
                                        break;
                                }
#ifndef OMIT_MEMCPY
                                /* copy page to ckpt_page */
                                if (pmo != page->pmo) {
                                        page->pmo = pmo;
                                }
                                ckpt_ret = ckpt_nvm_page(
                                        pmo, (void *)phys_to_virt(pa), index);
#endif /* OMIT_MEMCPY */
                                /* Add pte patch */
                                add_pte_patch_to_pool(vmspace, pte, page);
#ifdef HYBRID_MEM
                                if (!ckpt_ret)
                                        track_access(page);
#else
                                UNUSED(ckpt_ret);
#endif /* HYBRID_MEM */
#ifdef REPORT_RUNTIME
                                pf_count++;
                                pf_tot_time += stop();
                                LOG("[ckpt=%llu] [page fault count] page=%p, pte=%p\n",
                                    CKPT_VERSION_NUMBER,
                                    page,
                                    pte);
#endif
                        }
#else
                        UNUSED(ckpt_ret);
#endif
#endif /* CHCORE_SLS */
                }
                break;
        }
        case PMO_FORBID: {
                kinfo("Forbidden memory access (pmo->type is PMO_FORBID).\n");
                BUG_ON(1);
                sys_exit_group(-1);
                break;
        }
        default: {
                kinfo("handle_trans_fault: faulting vmr->pmo->type"
                      "(pmo type %d at 0x%lx)\n",
                      vmr->pmo->type,
                      fault_addr);
                kinfo("Currently, this pmo type should not trigger pgfaults\n");
                kprint_vmr(vmspace);
                BUG_ON(1);
                return -ENOMAPPING;
        }
        }

        read_unlock(&vmspace->vmspace_lock);
        return ret;
}

#endif
