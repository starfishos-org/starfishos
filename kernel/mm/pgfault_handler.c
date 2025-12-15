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
#include <mm/page_table_func.h>
#include <object/recycle.h>
#ifdef DSM_ENABLED
#include <dsm/dsm-single.h>
#include <lib/fw_cfg.h>
#endif
#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
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

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
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
    vmr_prop_t perm;
#if defined CHCORE_SLS
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
        kinfo("handle_trans_fault: no vmr found for va 0x%lx!\n", fault_addr);
        return -ENOMAPPING;
    }

    pmo = vmr->pmo;

    if (pmo->type > PMO_TYPE_NR || pmo->type < 0) {
        kinfo("handle_trans_fault: faulting vmr->pmo->type (pmo type %d at 0x%lx)\n",
              vmr->pmo->type, fault_addr);
        kinfo("Currently, this pmo type should not trigger pgfaults\n");
        kprint_vmr(vmspace);
        ret = -ENOMAPPING;
        goto out_unlock_vmspace;
    }

    if (pmo->type == PMO_FORBID) {
        kinfo("handle_trans_fault: forbidden memory access (pmo->type is PMO_FORBID).\n");
        BUG_ON(1);
        sys_exit_group(-1);
        ret = -EINVAL;
        goto out_unlock_vmspace;
    }


    /* A valid pmo, should handle page fault */
    perm = vmr->perm;

    /* Get the offset in the pmo for faulting addr */
    offset = ROUND_DOWN(fault_addr, PAGE_SIZE) - vmr->start;

    /* Boundary check */
    if ((offset >= pmo->size) && (pmo->type == PMO_FILE)) {
        kwarn_once("%s (out-of-range writing) offset 0x%lx, pmo->size 0x%lx, FILE\n",
                __func__, offset, pmo->size);
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
        /* Not committed before. Then, allocate the physical page. */
#ifdef MULTI_PAGETABLE_ENABLED
        /* If MULTI_PAGETABLE is enabled, we need to allocate
         * a private page for the new mapping. Only when another
         * machine also want to access this page, the page will
         * be shared. */
        void *new_va = get_pages(0, __MT_PRIVATE__);
        /* Otherwise, allocate according to the pmo's mm_type */
#else
        void *new_va = get_pages(0, pmo->mm_type);
#endif
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
#ifdef MULTI_PAGETABLE_ENABLED
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        map_page_in_pgtbl(pgtbl, fault_addr, pa, perm, &pte);
#else
        map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
#endif
        unlock(&vmspace->pgtbl_lock);

#if defined CHCORE_SLS || defined CHCORE_SSI_SLS
        /* do not persist pages belong to external sync pmo */
        if (is_external_sync_pmo(pmo))
            goto out_unlock_vmspace;
#ifdef OMIT_BENCHMARK
        /* omit track page fault of benchmarks */
        if (is_benchmark_vmspace(vmspace)) {
            goto out_unlock_vmspace;
        }
#endif
#ifndef OMIT_PF
        page = virt_to_page(new_va);
        if ((vmspace->flags & VM_FLAG_PRESERVE) && !is_external_sync_pmo(pmo)) {
#ifdef PMO_CHECKSUM
            page->ckpt_version_number = get_current_ckpt_version() + 1;
#endif
#ifndef OMIT_BENCHMARK
#ifdef CHCORE_SSI_SLS
            ckpt_dsm_page(pmo, new_va, index);
#else
            ckpt_nvm_page(pmo, new_va, index);
#endif
#endif
            add_pte_patch_to_pool(vmspace, (pte_t *)pte, page);
        }
#endif
#endif /* CHCORE_SLS */
    } else {
        /**
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

#if defined CHCORE_SLS && !defined(OMIT_PF)
        if ((vmspace->flags & VM_FLAG_PRESERVE) && !write
            && !is_external_sync_pmo(pmo)) {
            /* Read preserved page, map as read-only */
            perm &= ~VMR_WRITE;
        }
#endif /* CHCORE_SLS */
        
        /* handle CoW when a process is forked */
#ifdef CHCORE_FORK_ENABLED
#ifdef MULTI_PAGETABLE_ENABLED
#error "Multi-PAGETABLE_ENABLED is not supported for CoW"
#endif
        if (!is_shared_pmo(pmo)) {
            if (is_continuous_pmo(pmo)) {
                page = virt_to_page((void *)phys_to_virt(pmo->start));
                
                lock(&page->lock);
                if (page->ref_cnt > 1) {
                    void *new_va = kmalloc(pmo->size, pmo->mm_type);
                    if (new_va == NULL) {
                        unlock(&page->lock);
                        return -ENOMEM;
                    }

                    memcpy(new_va, (void *)phys_to_virt(pmo->start), pmo->size);
                    pmo->start = virt_to_phys(new_va);
                    /* new pa */
                    pa = pmo->start + index * PAGE_SIZE;

                    lock(&vmspace->pgtbl_lock);
                    void *pgtbl = vmspace->pgtbl;
                    if ((vmspace->flags & VM_FLAG_PRESERVE)) {
                        map_range_in_pgtbl(pgtbl,
                                            vmr->start,
                                            pmo->start,
                                            pmo->size,
                                            perm & (~VMR_WRITE));
                    } else {
                        map_range_in_pgtbl(pgtbl,
                                            vmr->start,
                                            pmo->start,
                                            pmo->size,
                                            perm);
                    }
                    unlock(&vmspace->pgtbl_lock);

                    flush_tlbs(vmspace, vmr->start, vmr->size);
                    atomic_fetch_sub_64(&page->ref_cnt, 1);
                }
                unlock(&page->lock);
            } else {
                int cow = false;
                page = virt_to_page((void *)phys_to_virt(pa));
                lock(&page->lock);
                if (page->ref_cnt > 1) {
                    void *new_va = get_pages(0, pmo->mm_type);
                    if (new_va == NULL) {
                        unlock(&page->lock);
                        return -ENOMEM;
                    }

                    pagecpy_nt(new_va, (void *)phys_to_virt(pa));
                    /* new pa */
                    pa = virt_to_phys(new_va);

                    lock(&vmspace->pgtbl_lock);
                    map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
                    unlock(&vmspace->pgtbl_lock);

                    flush_tlbs(vmspace, fault_addr, PAGE_SIZE);
                    atomic_fetch_sub_64(&page->ref_cnt, 1);
                    cow = true;
                }
                unlock(&page->lock);
                if (cow) {
                    commit_page_to_pmo(pmo, index, pa);
                    unlock(&page->lock);
                    return 0;
                }
            }
        }
#endif /* CHCORE_FORK_ENABLED */

        /* Add mapping in the page table */
        pte_t *pte = NULL;
        lock(&vmspace->pgtbl_lock);
#ifdef MULTI_PAGETABLE_ENABLED
        /**
         * If MULTI_PAGETABLE is enabled, we need to map all 
         * page tables to the shared memory.
         * Case1: the page is already on shared memory
         *        -- DO NOTHING! Directly map!
         * Case2: the page is not on shared memory
         *        -- 2.1: memcpy the page to the shared memory.
         *        -- 2.2: remap the page table in old page table.
         *        -- 2.3: map the page to new page table.
         */
        /* NOTE!!: should not define machine_id variable here, 
        it will cause the error when using CUR_MACHINE_ID */
        int mid = get_paddr_machine_id(pa);
        BUG_ON(mid == MACHINE_ID_INVALID);
        if (mid != MACHINE_ID_SHARED_MEMORY) {
            pte_t *pte_entry = NULL;
            paddr_t old_pa = pa;
            query_in_pgtbl(vmspace->pgtbl[mid], fault_addr, &old_pa, &pte_entry);
            BUG_ON(old_pa == 0);
            BUG_ON(pte_entry == NULL);
            /* 2.1: memcpy the page to the shared memory. */
            memcpy((void *)phys_to_virt(pa), (void *)phys_to_virt(old_pa), PAGE_SIZE);
            /* 2.2: remap the page table in old page table. */
            remap_page_in_pgtbl(pte_entry, pa);
            /* Flush TLB only for the machine whose page table was remapped */
            /* Note: We need to flush because remap changes the physical address,
             * and TLB may have cached the old mapping. We only flush CPUs belonging
             * to machine_id since only they may have cached mappings from that page table */
            flush_tlbs_for_machine(vmspace, mid, fault_addr, PAGE_SIZE);
        }
        
        void *pgtbl = get_vmspace_pgtbl(vmspace, CUR_MACHINE_ID);
        map_page_in_pgtbl(pgtbl, fault_addr, pa, perm, &pte);
#else
        map_page_in_pgtbl(vmspace->pgtbl, fault_addr, pa, perm, &pte);
#endif
        unlock(&vmspace->pgtbl_lock);

#ifdef CHCORE_SLS        
        /* do not persist pages belong to external sync pmo */
        if (is_external_sync_pmo(pmo))
            return 0;
#ifndef OMIT_PF
#ifdef OMIT_BENCHMARK
        /* omit track page fault of benchmarks */
        if (is_benchmark_vmspace(vmspace)) {
            return 0;
        }
#endif

        if (write && (vmspace->flags & VM_FLAG_PRESERVE)) {
            page = virt_to_page((void *)phys_to_virt(pa));
            BUG_ON(unlikely(!page));
            if (unlikely(get_page_type(page) != NVM_PAGE)) {
                /* Dram page will be marked as unwritable after fork */
                unlock(&page->lock);
                return -ENOMEM;
            }
#ifndef OMIT_MEMCPY
            /* copy page to ckpt_page */
            if (pmo != page->pmo) {
                page->pmo = pmo;
            }

            ckpt_ret = ckpt_nvm_page(pmo, (void *)phys_to_virt(pa), index);

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

out_unlock_vmspace:
    read_unlock(&vmspace->vmspace_lock);
    return ret;
}

#endif
